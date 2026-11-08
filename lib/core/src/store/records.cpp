// Binary record codecs + compact JSON rows. Pure. Owner: store.
#include "store/records.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace rec {

namespace {
void p16(uint8_t* o, uint16_t v) { o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8); }
void p32(uint8_t* o, uint32_t v) { p16(o, (uint16_t)v); p16(o + 2, (uint16_t)(v >> 16)); }
uint16_t g16(const uint8_t* i) { return (uint16_t)(i[0] | (i[1] << 8)); }
uint32_t g32(const uint8_t* i) { return g16(i) | ((uint32_t)g16(i + 2) << 16); }
bool blank(uint32_t t) { return t == 0 || t == 0xFFFFFFFFu; }

// "12.3" / "null" (b needs >= 12 bytes)
const char* tenth(char* b, int16_t v) {
  if (v == kNoTenths) return "null";
  int a = v < 0 ? -v : v;
  snprintf(b, 12, "%s%d.%d", v < 0 ? "-" : "", a / 10, a % 10);
  return b;
}
const char* hund(char* b, uint32_t v) { snprintf(b, 12, "%u.%02u", (unsigned)(v / 100), (unsigned)(v % 100)); return b; }
const char* soc(char* b, uint8_t v) {
  if (v > 100) return "null";
  snprintf(b, 12, "%u", v);
  return b;
}
size_t out(size_t cap, int n) { return (n > 0 && (size_t)n < cap) ? (size_t)n : 0; }
}  // namespace

int16_t toTenths(float v) {
  if (isnan(v)) return kNoTenths;
  float t = roundf(v * 10.0f);
  if (t > 32000) t = 32000;
  if (t < -32000) t = -32000;
  return (int16_t)t;
}
float fromTenths(int16_t v) { return v == kNoTenths ? NAN : v / 10.0f; }

void encode(const Hourly& r, uint8_t o[kHourlySize]) {
  p32(o, r.hour_epoch);
  p16(o + 4, (uint16_t)r.price10);
  p16(o + 6, (uint16_t)r.dayahead10);
  o[8] = r.relay_on_min;
  p16(o + 9, r.kwh100);
  o[11] = r.soc_start;
  o[12] = r.soc_end;
  p16(o + 13, r.avg_kw100);
  o[15] = r.flags;
}
bool decode(const uint8_t i[kHourlySize], Hourly& r) {
  r.hour_epoch = g32(i);
  if (blank(r.hour_epoch)) return false;
  r.price10 = (int16_t)g16(i + 4);
  r.dayahead10 = (int16_t)g16(i + 6);
  r.relay_on_min = i[8];
  r.kwh100 = g16(i + 9);
  r.soc_start = i[11];
  r.soc_end = i[12];
  r.avg_kw100 = g16(i + 13);
  r.flags = i[15];
  return true;
}

void encode(const Session& r, uint8_t o[kSessionSize]) {
  memset(o, 0, kSessionSize);
  p32(o, r.start);
  p32(o + 4, r.end);
  o[8] = r.soc_start;
  o[9] = r.soc_end;
  p16(o + 10, r.kwh100);
  p32(o + 12, (uint32_t)r.cost_c10);
  p16(o + 16, (uint16_t)r.avg_price10);
  p16(o + 18, r.max_kw100);
  o[20] = r.flags;
}
bool decode(const uint8_t i[kSessionSize], Session& r) {
  r.start = g32(i);
  if (blank(r.start)) return false;
  r.end = g32(i + 4);
  r.soc_start = i[8];
  r.soc_end = i[9];
  r.kwh100 = g16(i + 10);
  r.cost_c10 = (int32_t)g32(i + 12);
  r.avg_price10 = (int16_t)g16(i + 16);
  r.max_kw100 = g16(i + 18);
  r.flags = i[20];
  return true;
}

void encode(const Detail& r, uint8_t o[kDetailSize]) {
  p32(o, r.epoch);
  p16(o + 4, (uint16_t)r.price10);
  p16(o + 6, (uint16_t)r.watts);
  o[8] = r.soc;
  o[9] = r.relay;
  o[10] = r.car_status;
  o[11] = r.flags;
}
bool decode(const uint8_t i[kDetailSize], Detail& r) {
  r.epoch = g32(i);
  if (blank(r.epoch)) return false;
  r.price10 = (int16_t)g16(i + 4);
  r.watts = (int16_t)g16(i + 6);
  r.soc = i[8];
  r.relay = i[9];
  r.car_status = i[10];
  r.flags = i[11];
  return true;
}

// [hour_epoch, price, dayahead, relay_on_min, kwh, soc_start, soc_end, avg_kw, flags]
size_t toJsonRow(const Hourly& r, char* buf, size_t cap) {
  char a[12], b[12], c[12], d[12], e[12], f[12];
  return out(cap, snprintf(buf, cap, "[%u,%s,%s,%u,%s,%s,%s,%s,%u]", (unsigned)r.hour_epoch, tenth(a, r.price10),
                           tenth(b, r.dayahead10), r.relay_on_min, hund(c, r.kwh100), soc(d, r.soc_start),
                           soc(e, r.soc_end), hund(f, r.avg_kw100), r.flags));
}

size_t toJsonRow(const Session& r, char* buf, size_t cap) {
  char a[12], b[12], c[12], d[12], e[12], f[12];
  int32_t cc = r.cost_c10;
  uint32_t ac = (uint32_t)(cc < 0 ? -(int64_t)cc : cc);
  snprintf(b, sizeof(b), "%s%u.%u", cc < 0 ? "-" : "", (unsigned)(ac / 10), (unsigned)(ac % 10));
  return out(cap,
             snprintf(buf, cap,
                      "{\"start\":%u,\"end\":%u,\"soc_start\":%s,\"soc_end\":%s,\"kwh\":%s,\"cost_c\":%s,"
                      "\"avg_price\":%s,\"max_kw\":%s,\"car_detected\":%s,\"charged\":%s,\"manual\":%s}",
                      (unsigned)r.start, (unsigned)r.end, soc(a, r.soc_start), soc(c, r.soc_end), hund(d, r.kwh100), b,
                      tenth(e, r.avg_price10), hund(f, r.max_kw100), (r.flags & kSessCarDetected) ? "true" : "false",
                      (r.flags & kSessCharged) ? "true" : "false", (r.flags & kSessManual) ? "true" : "false"));
}

// [epoch, price, watts, soc, relay, car_status]
size_t toJsonRow(const Detail& r, char* buf, size_t cap) {
  char a[12], b[12], c[12];
  if (r.watts == kNoTenths) snprintf(b, sizeof(b), "null");
  else snprintf(b, sizeof(b), "%d", r.watts);
  return out(cap, snprintf(buf, cap, "[%u,%s,%s,%s,%u,%u]", (unsigned)r.epoch, tenth(a, r.price10), b, soc(c, r.soc),
                           r.relay ? 1 : 0, r.car_status));
}

}  // namespace rec
