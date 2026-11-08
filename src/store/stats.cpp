// Stats + history on LittleFS. Owner: store.
//
// Each history kind is a "segmented ring": append-only files /<prefix><seq>.bin holding `perSeg`
// fixed-size records; when the newest is full a new one starts and the oldest beyond `maxSegs` is
// deleted. (Append-only on purpose: LittleFS rewrites the tail of a file when you overwrite in the
// middle, which would be slow, wear flash and need free space.) Records are time ordered.
// Sized for the 320 KB LittleFS partition (app slots were grown to 1.81 MB):
//   hourly   /hr<n>.bin  16 B x  512 x 16 segs  ~ 11 months  (~128 KB max)
//   detail   /dt<n>.bin  12 B x 1024 x  5 segs  ~ 17 days    (~60 KB max), 5-min records batched 12 at a time
//   sessions /ss<n>.bin  32 B x  256 x  4 segs  = 1024       (~32 KB max)
// If free space drops under 48 KB when a new segment starts, the oldest hourly segment is dropped.
// A crash loses <= 1 h of detail and the in-progress hour/session. flush() (reboot/OTA) writes the
// in-progress session as finished and the partial hour as its own record (hourly rows are additive).
#include "store/stats.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace stats {

namespace {

struct Lock {
  static SemaphoreHandle_t h() { static SemaphoreHandle_t m = xSemaphoreCreateMutex(); return m; }
  Lock() { xSemaphoreTake(h(), portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(h()); }
};

struct Ring {
  const char* prefix;
  uint8_t rs;
  uint16_t perSeg;
  uint8_t maxSegs;
  uint8_t pendCap;       // detail only: records batched in RAM
  uint8_t* pend;
  bool have = false;
  uint32_t seqMin = 0, seqMax = 0;
  uint16_t cnt = 0;      // records in the newest segment
  uint8_t pendN = 0;
};

uint8_t g_detPend[12 * rec::kDetailSize];
Ring g_hourly{"hr", rec::kHourlySize, 512, 16, 0, nullptr};
Ring g_detail{"dt", rec::kDetailSize, 1024, 5, 12, g_detPend};
Ring g_sess{"ss", rec::kSessionSize, 256, 4, 0, nullptr};

void segPath(const Ring& r, uint32_t seq, char* out) { snprintf(out, 24, "/%s%u.bin", r.prefix, (unsigned)seq); }
uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint32_t segCount(const Ring& r, uint32_t seq) { return seq == r.seqMax ? r.cnt : r.perSeg; }

void scan(Ring& r) {
  File root = LittleFS.open("/");
  if (!root) return;
  size_t pl = strlen(r.prefix);
  uint32_t mn = 0xFFFFFFFFu, mx = 0;
  bool any = false;
  for (File e = root.openNextFile(); e; e = root.openNextFile()) {
    const char* n = e.name();
    const char* sl = strrchr(n, '/');
    if (sl) n = sl + 1;
    if (strncmp(n, r.prefix, pl) != 0 || !isdigit((unsigned char)n[pl])) continue;
    char* end;
    uint32_t q = (uint32_t)strtoul(n + pl, &end, 10);
    if (strcmp(end, ".bin") != 0) continue;
    any = true;
    if (q < mn) mn = q;
    if (q > mx) mx = q;
  }
  root.close();
  if (!any) return;
  r.have = true;
  r.seqMin = mn;
  r.seqMax = mx;
  char p[24];
  segPath(r, mx, p);
  File f = LittleFS.open(p, "r");
  r.cnt = f ? (uint16_t)(f.size() / r.rs) : 0;
  if (r.cnt > r.perSeg) r.cnt = r.perSeg;
}

void dropOldest(Ring& r) {
  char p[24];
  segPath(r, r.seqMin, p);
  LittleFS.remove(p);
  r.seqMin++;
}

void newSeg(Ring& r) {
  if (!r.have) { r.have = true; r.seqMin = r.seqMax = 1; }
  else r.seqMax++;
  r.cnt = 0;
  while (r.seqMax - r.seqMin + 1 > r.maxSegs) dropOldest(r);
  while (LittleFS.totalBytes() - LittleFS.usedBytes() < 48 * 1024 && g_hourly.have && g_hourly.seqMax > g_hourly.seqMin)
    dropOldest(g_hourly);
}

void appendRecs(Ring& r, const uint8_t* d, size_t n) {
  while (n) {
    if (!r.have || r.cnt >= r.perSeg) newSeg(r);
    size_t k = r.perSeg - r.cnt;
    if (k > n) k = n;
    char p[24];
    segPath(r, r.seqMax, p);
    File f = LittleFS.open(p, FILE_APPEND);
    if (!f) return;
    size_t w = f.write(d, k * r.rs);
    f.close();
    if (w != k * r.rs) return;
    r.cnt += k;
    d += k * r.rs;
    n -= k;
  }
}

void flushPend(Ring& r) {
  if (r.pendN) { appendRecs(r, r.pend, r.pendN); r.pendN = 0; }
}

// ---------------------------------------------------------------- accumulators
struct HourAcc {
  bool active = false;
  uint32_t start = 0;
  float psum = 0;
  uint16_t pn = 0;
  int16_t da10 = rec::kNoTenths;
  uint32_t relay_s = 0;
  float kwh = 0, kwsum = 0;
  uint16_t kwn = 0;
  uint8_t soc0 = 0xFF, soc1 = 0xFF, flags = 0;
} g_h;

struct SessAcc {
  bool active = false;
  uint32_t start = 0;
  uint8_t soc0 = 0xFF, flags = 0;
  float kwh = 0, cost_c = 0, maxkw = 0, psum = 0;
  uint16_t pn = 0;
} g_s;

rec::Session g_last;
bool g_haveLast = false;
int64_t g_lastT = 0;
bool g_prevRelay = false;
uint32_t g_lastSlot = 0;

uint16_t clamp16(float v) { return v <= 0 ? 0 : v >= 65535 ? 65535 : (uint16_t)lroundf(v); }
uint8_t socByte(float s) { return isnan(s) ? 0xFF : (uint8_t)(s < 0 ? 0 : s > 100 ? 100 : lroundf(s)); }

bool hourRec(rec::Hourly& h) {
  if (!g_h.active || (g_h.pn == 0 && g_h.relay_s == 0 && g_h.kwh <= 0)) return false;
  h = rec::Hourly();
  h.hour_epoch = g_h.start;
  h.price10 = g_h.pn ? rec::toTenths(g_h.psum / g_h.pn) : rec::kNoTenths;
  h.dayahead10 = g_h.da10;
  uint32_t m = (g_h.relay_s + 30) / 60;
  h.relay_on_min = (uint8_t)(m > 60 ? 60 : m);
  h.kwh100 = clamp16(g_h.kwh * 100);
  h.soc_start = g_h.soc0;
  h.soc_end = g_h.soc1;
  h.avg_kw100 = g_h.kwn ? clamp16(g_h.kwsum / g_h.kwn * 100) : 0;
  h.flags = g_h.flags;
  return true;
}

void finishHour() {
  rec::Hourly h;
  if (hourRec(h)) {
    uint8_t b[rec::kHourlySize];
    rec::encode(h, b);
    appendRecs(g_hourly, b, 1);
  }
  g_h = HourAcc();
}

void sessRec(rec::Session& s, uint32_t end) {
  s = rec::Session();
  s.start = g_s.start;
  s.end = end;
  s.soc_start = g_s.soc0;
  s.kwh100 = clamp16(g_s.kwh * 100);
  s.cost_c10 = (int32_t)lroundf(g_s.cost_c * 10);
  s.avg_price10 = g_s.kwh > 0.01f ? rec::toTenths(g_s.cost_c / g_s.kwh) : g_s.pn ? rec::toTenths(g_s.psum / g_s.pn) : rec::kNoTenths;
  s.max_kw100 = clamp16(g_s.maxkw * 100);
  s.flags = g_s.flags;
}

void endSession(uint32_t end, uint8_t soc_end) {
  if (!g_s.active) return;
  if (end >= g_s.start + 30) {
    rec::Session s;
    sessRec(s, end);
    s.soc_end = soc_end;
    uint8_t b[rec::kSessionSize];
    rec::encode(s, b);
    appendRecs(g_sess, b, 1);
    g_last = s;
    g_haveLast = true;
  }
  g_s = SessAcc();
}

// ---------------------------------------------------------------- reading
// Row formatters: return bytes, 0 = didn't fit.
typedef size_t (*RowFn)(const uint8_t*, char*, size_t);
size_t rowH(const uint8_t* b, char* o, size_t c) { rec::Hourly r; return rec::decode(b, r) ? rec::toJsonRow(r, o, c) : 0; }
size_t rowD(const uint8_t* b, char* o, size_t c) { rec::Detail r; return rec::decode(b, r) ? rec::toJsonRow(r, o, c) : 0; }

size_t ramCount(const Ring& r) {
  if (&r == &g_hourly) { rec::Hourly h; return hourRec(h) ? 1 : 0; }
  return r.pendN;
}
bool ramGet(const Ring& r, size_t j, uint8_t* out) {
  if (&r == &g_hourly) {
    rec::Hourly h;
    if (!hourRec(h)) return false;
    rec::encode(h, out);
    return true;
  }
  memcpy(out, r.pend + j * r.rs, r.rs);
  return true;
}

constexpr uint32_t kEmitted = 0x80000000u, kRamPhase = 0x40000000u, kPosMask = 0x3FFFFFFFu;

// Rows with from <= ts <= to, comma-joined, appended to buf. Cursor = flags | position (file abs
// index = seq*perSeg+idx, or RAM index). Returns bytes written; 0 = finished.
size_t fill(Ring& r, uint32_t from, uint32_t to, uint32_t& cursor, char* buf, size_t cap, RowFn fn) {
  Lock l;
  size_t w = 0;
  bool emitted = cursor & kEmitted, ram = cursor & kRamPhase;
  uint32_t pos = cursor & kPosMask;
  uint8_t b[rec::kSessionSize];
  bool stop = false;

  // returns false to stop scanning (cursor is left pointing at the row that stopped us)
  auto take = [&](uint32_t where) -> bool {
    uint32_t ts = le32(b);
    if (ts == 0 || ts == 0xFFFFFFFFu) return true;
    if (ts < from) return true;
    size_t k = 0;
    if (ts <= to && cap > w + 2) k = fn(b, buf + w + (emitted ? 1 : 0), cap - w - (emitted ? 1 : 0));
    if (!k) {
      cursor = (emitted ? kEmitted : 0) | (ram ? kRamPhase : 0) | where;
      stop = true;
      return false;
    }
    if (emitted) buf[w] = ',';
    w += k + (emitted ? 1 : 0);
    emitted = true;
    return true;
  };

  if (!ram && r.have) {
    uint32_t seq = pos / r.perSeg, idx = pos % r.perSeg;
    if (seq < r.seqMin) { seq = r.seqMin; idx = 0; }
    for (; seq <= r.seqMax && !stop; seq++, idx = 0) {
      uint32_t n = segCount(r, seq);
      char p[24];
      segPath(r, seq, p);
      File f = LittleFS.open(p, "r");
      if (!f) continue;
      if (idx == 0 && n > 0) {   // skip a whole segment that ends before `from`
        if (f.seek((n - 1) * r.rs) && f.read(b, r.rs) == r.rs && le32(b) < from && le32(b) != 0) continue;
      }
      if (!f.seek(idx * r.rs)) continue;
      for (; idx < n; idx++) {
        if (f.read(b, r.rs) != r.rs) break;
        if (!take(seq * r.perSeg + idx)) break;
      }
    }
    if (stop) return w;
    pos = 0;
  }
  ram = true;
  size_t nr = ramCount(r);
  for (size_t j = ram && (cursor & kRamPhase) ? pos : 0; j < nr && !stop; j++) {
    if (!ramGet(r, j, b)) continue;
    if (!take((uint32_t)j)) break;
  }
  if (stop) return w;
  cursor = (emitted ? kEmitted : 0) | kRamPhase | (uint32_t)nr;
  return w;
}

}  // namespace

// ---------------------------------------------------------------- public
void begin() {
  Lock l;
  scan(g_hourly);
  scan(g_detail);
  scan(g_sess);
  if (g_sess.have && g_sess.cnt) {
    char p[24];
    segPath(g_sess, g_sess.seqMax, p);
    File f = LittleFS.open(p, "r");
    uint8_t b[rec::kSessionSize];
    if (f && f.seek((g_sess.cnt - 1) * rec::kSessionSize) && f.read(b, sizeof(b)) == sizeof(b))
      g_haveLast = rec::decode(b, g_last);
  }
}

void sample(const ChicagoTime* now, const PriceSnapshot& p, const CarSnapshot& car, bool relay, Mode mode) {
  Lock l;
  if (!now || now->epoch < kMinValidEpoch) { g_lastT = 0; g_prevRelay = relay; return; }
  const int64_t t = now->epoch;
  int64_t dt = g_lastT ? t - g_lastT : 0;
  if (dt < 0 || dt > 900) dt = 0;
  const uint32_t hs = (uint32_t)now->hourStart();

  if (g_h.active && g_h.start != hs) finishHour();
  if (!g_h.active) {
    g_h.active = true;
    g_h.start = hs;
    uint32_t today = now->yyyymmdd();
    if (p.da_today_date == today) g_h.da10 = rec::toTenths(p.da_today[(now->hour + 1) % 24]);
  }

  const bool charging = car.status == CarStatus::Charging && !isnan(car.watts);
  const float kw = charging ? fabsf(car.watts) / 1000.0f : 0;
  const bool priceKnown = !isnan(p.current);
  const uint8_t soc = socByte(car.soc);
  const bool seen = car.status != CarStatus::Unknown && car.status != CarStatus::NotDetected;

  // integrate over the interval that just ended, using the relay state that held during it
  if (g_prevRelay && dt > 0) {
    g_h.relay_s += (uint32_t)dt;
    if (charging) {
      float e = kw * dt / 3600.0f;
      g_h.kwh += e;
      if (g_s.active) {
        g_s.kwh += e;
        if (priceKnown) g_s.cost_c += e * p.current;
      }
    }
  }
  if (priceKnown) { g_h.psum += p.current; g_h.pn++; }
  if (charging) { g_h.kwsum += kw; g_h.kwn++; }
  if (soc != 0xFF) { if (g_h.soc0 == 0xFF) g_h.soc0 = soc; g_h.soc1 = soc; }
  g_h.flags = (uint8_t)(((g_h.flags & 0x18) | ((uint8_t)mode & 7)) | (seen ? 8 : 0) | (car.status == CarStatus::Charging ? 16 : 0));

  // sessions
  if (relay && !g_s.active) {
    g_s = SessAcc();
    g_s.active = true;
    g_s.start = (uint32_t)t;
    g_s.soc0 = soc;
    if (mode == Mode::ManualOn) g_s.flags |= rec::kSessManual;
  }
  if (g_s.active) {
    if (priceKnown) { g_s.psum += p.current; g_s.pn++; }
    if (kw > g_s.maxkw) g_s.maxkw = kw;
    if (seen) g_s.flags |= rec::kSessCarDetected;
    if (car.status == CarStatus::Charging) g_s.flags |= rec::kSessCharged;
  }
  if (!relay && g_s.active) endSession((uint32_t)t, soc);

  // 5-minute detail
  uint32_t slot = (uint32_t)(t / 300);
  if (slot > g_lastSlot) {
    g_lastSlot = slot;
    rec::Detail d;
    d.epoch = slot * 300;
    d.price10 = rec::toTenths(p.current);
    d.watts = isnan(car.watts) ? rec::kNoTenths : (int16_t)(car.watts > 32000 ? 32000 : car.watts < -32000 ? -32000 : car.watts);
    d.soc = soc;
    d.relay = relay ? 1 : 0;
    d.car_status = (uint8_t)car.status;
    d.flags = p.estimated ? 1 : 0;
    rec::encode(d, g_detail.pend + g_detail.pendN * rec::kDetailSize);
    if (++g_detail.pendN >= g_detail.pendCap) flushPend(g_detail);
  }

  g_prevRelay = relay;
  g_lastT = t;
}

void flush() {
  Lock l;
  if (g_s.active && g_lastT) endSession((uint32_t)g_lastT, socByte(NAN));
  finishHour();
  flushPend(g_detail);
}

bool currentSession(rec::Session& out) {
  Lock l;
  if (!g_s.active) return false;
  sessRec(out, 0);
  return true;
}

bool lastSession(rec::Session& out) {
  Lock l;
  if (!g_haveLast) return false;
  out = g_last;
  return true;
}

size_t fillHourly(uint32_t from, uint32_t to, uint32_t& cursor, char* buf, size_t cap) {
  return fill(g_hourly, from, to, cursor, buf, cap, rowH);
}
size_t fillDetail(uint32_t from, uint32_t to, uint32_t& cursor, char* buf, size_t cap) {
  return fill(g_detail, from, to, cursor, buf, cap, rowD);
}

size_t fillSessions(uint16_t limit, uint32_t& cursor, char* buf, size_t cap) {
  Lock l;
  size_t w = 0;
  if (!g_sess.have) return 0;
  uint32_t total = (g_sess.seqMax - g_sess.seqMin) * g_sess.perSeg + g_sess.cnt;
  uint32_t k = cursor & kPosMask;   // rows already emitted (newest first)
  bool emitted = cursor & kEmitted;
  uint8_t b[rec::kSessionSize];
  File f;
  uint32_t openSeq = 0xFFFFFFFFu;
  while (k < limit && k < total) {
    uint32_t g = total - 1 - k;
    uint32_t seq = g_sess.seqMin + g / g_sess.perSeg, idx = g % g_sess.perSeg;
    if (seq != openSeq) {
      if (f) f.close();
      char p[24];
      segPath(g_sess, seq, p);
      f = LittleFS.open(p, "r");
      openSeq = seq;
    }
    rec::Session s;
    if (f && f.seek(idx * rec::kSessionSize) && f.read(b, sizeof(b)) == sizeof(b) && rec::decode(b, s)) {
      if (emitted) { if (cap <= w + 1) break; buf[w] = ','; }
      size_t n = rec::toJsonRow(s, buf + w + (emitted ? 1 : 0), cap - w - (emitted ? 1 : 0));
      if (!n) break;
      w += n + (emitted ? 1 : 0);
      emitted = true;
    }
    k++;
  }
  cursor = (emitted ? kEmitted : 0) | k;
  return w;
}

namespace {
size_t aggregate(bool monthly, uint16_t n, uint32_t hoursPerBin, Agg* out, size_t max) {
  constexpr size_t kMaxBins = 62;   // daily API allows up to 62 days
  if (n > max) n = (uint16_t)max;
  if (n > kMaxBins) n = kMaxBins;
  if (!n) return 0;
  Lock l;
  float relmin[kMaxBins];
  size_t cnt = 0;
  auto add = [&](const rec::Hourly& h) {
    ChicagoTime c = chicagoFromEpoch(h.hour_epoch);
    uint32_t key = monthly ? (uint32_t)c.year * 100 + c.month : c.yyyymmdd();
    if (!cnt || out[cnt - 1].key != key) {
      if (cnt == n) {
        memmove(out, out + 1, (n - 1) * sizeof(Agg));
        memmove(relmin, relmin + 1, (n - 1) * sizeof(float));
        cnt--;
      }
      out[cnt] = Agg{key, 0, 0, 0};
      relmin[cnt++] = 0;
    }
    Agg& a = out[cnt - 1];
    float kwh = h.kwh100 / 100.0f, p = rec::fromTenths(h.price10);
    a.kwh += kwh;
    if (!isnan(p)) a.cost_c += kwh * p;   // supply cost approximated with the hour's average price
    relmin[cnt - 1] += h.relay_on_min;
  };
  if (g_hourly.have) {
    uint32_t need = (n * hoursPerBin + 24 + g_hourly.perSeg - 1) / g_hourly.perSeg + 1;
    uint32_t avail = g_hourly.seqMax - g_hourly.seqMin + 1;
    uint32_t seq = g_hourly.seqMin + (avail > need ? avail - need : 0);
    uint8_t blk[8 * rec::kHourlySize];
    for (; seq <= g_hourly.seqMax; seq++) {
      char p[24];
      segPath(g_hourly, seq, p);
      File f = LittleFS.open(p, "r");
      if (!f) continue;
      int got;
      while ((got = f.read(blk, sizeof(blk))) >= (int)rec::kHourlySize)
        for (int o = 0; o + (int)rec::kHourlySize <= got; o += rec::kHourlySize) {
          rec::Hourly h;
          if (rec::decode(blk + o, h)) add(h);
        }
    }
  }
  rec::Hourly cur;
  if (hourRec(cur)) add(cur);
  for (size_t i = 0; i < cnt; i++) out[i].relay_h10 = (uint16_t)lroundf(relmin[i] / 6.0f);
  return cnt;
}
}  // namespace

size_t daily(uint16_t days, Agg* out, size_t max) { return aggregate(false, days, 24, out, max); }
size_t monthly(uint16_t months, Agg* out, size_t max) { return aggregate(true, months, 31 * 24, out, max); }

FsInfo fsInfo() { return FsInfo{LittleFS.totalBytes(), LittleFS.usedBytes()}; }

}  // namespace stats
