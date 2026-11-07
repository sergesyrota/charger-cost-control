// Event log: RAM tail (fast) + /events.log on LittleFS (last ~500 lines). Owner: store.
// File line: "<epoch>\t<uptime>\t<kind digit>\t<message>\n". Appended per event, trimmed in bulk.
#include "store/event_log.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "net/clock.h"

namespace evlog {

namespace {
constexpr const char* kPath = "/events.log";
constexpr const char* kTmp = "/events.tmp";
constexpr uint32_t kKeep = 500, kSlack = 100;   // trim to kKeep when the file reaches kKeep+kSlack
constexpr uint32_t kRam = 32;
constexpr size_t kMsg = 160;

struct Slot {
  int64_t t;
  uint32_t up;
  Kind k;
  char msg[kMsg + 1];
};

Slot g_ram[kRam];
uint32_t g_head = 0;      // total entries ever added
uint32_t g_file_lines = 0;
uint32_t g_gen = 0;       // bumped when the file is rewritten
bool g_fs = false;

struct Lock {
  static SemaphoreHandle_t h() { static SemaphoreHandle_t m = xSemaphoreCreateMutex(); return m; }
  Lock() { xSemaphoreTake(h(), portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(h()); }
};

bool matches(Kind k, const char* f) {
  if (!f || !*f) return true;
  const char* n = kindName(k);
  return strcmp(n, f) == 0 || (k == Kind::State && strcmp(f, "relay") == 0);
}

void appendFile(const Slot& s) {
  File f = LittleFS.open(kPath, FILE_APPEND);
  if (!f) return;
  f.printf("%lld\t%u\t%u\t%s\n", (long long)s.t, (unsigned)s.up, (unsigned)s.k, s.msg);
  f.close();
  g_file_lines++;
}

void trim() {
  File src = LittleFS.open(kPath, "r");
  File dst = LittleFS.open(kTmp, "w");
  if (!src || !dst) return;
  uint32_t skip = g_file_lines > kKeep ? g_file_lines - kKeep : 0;
  char b[256];
  while (src.available()) {
    size_t n = src.readBytesUntil('\n', b, sizeof(b) - 1);
    if (skip) { skip--; continue; }
    dst.write((const uint8_t*)b, n);
    dst.write('\n');
  }
  src.close();
  dst.close();
  LittleFS.remove(kPath);
  LittleFS.rename(kTmp, kPath);
  g_file_lines = kKeep;
  g_gen++;
}

bool parseLine(char* line, Entry& e) {
  char* end;
  e.t = strtoll(line, &end, 10);
  if (*end != '\t') return false;
  e.up = (uint32_t)strtoul(end + 1, &end, 10);
  if (*end != '\t') return false;
  unsigned k = (unsigned)strtoul(end + 1, &end, 10);
  if (*end != '\t' || k > (unsigned)Kind::Err) return false;
  e.k = (Kind)k;
  e.msg = end + 1;
  return true;
}
}  // namespace

const char* kindName(Kind k) {
  static const char* const n[] = {"sys", "relay", "state", "push", "car", "price", "err"};
  return n[(int)k];
}

void begin() {
  Lock l;
  g_fs = true;
  g_file_lines = 0;
  File f = LittleFS.open(kPath, "r");
  if (f) {
    uint8_t b[256];
    int n;
    while ((n = f.read(b, sizeof(b))) > 0)
      for (int i = 0; i < n; i++)
        if (b[i] == '\n') g_file_lines++;
    f.close();
  }
  // Entries logged before the FS was mounted only lived in RAM.
  uint32_t first = g_head > kRam ? g_head - kRam : 0;
  for (uint32_t q = first; q < g_head; q++) appendFile(g_ram[q % kRam]);
  if (g_file_lines >= kKeep + kSlack) trim();
}

void add(Kind k, const char* fmt, ...) {
  Slot s;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(s.msg, sizeof(s.msg), fmt, ap);
  va_end(ap);
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof(s.msg)) {   // truncated: don't cut a UTF-8 sequence in half
    n = sizeof(s.msg) - 1;
    while (n > 0 && ((uint8_t)s.msg[n] & 0xC0) == 0x80) n--;
    s.msg[n] = 0;
  }
  for (char* p = s.msg; *p; p++)
    if (*p == '\n' || *p == '\r' || *p == '\t') *p = ' ';
  s.t = sysclock::now();
  s.up = sysclock::uptimeS();
  s.k = k;
  Serial.printf("[%s] %s\n", kindName(k), s.msg);
  Lock l;
  g_ram[g_head % kRam] = s;
  g_head++;
  if (g_fs) {
    appendFile(s);
    if (g_file_lines >= kKeep + kSlack) trim();
  }
}

void forEach(size_t limit, const char* filter, const std::function<bool(const Entry&)>& cb) {
  size_t emitted = 0;
  uint32_t h;
  {
    Lock l;
    h = g_head;
  }
  uint32_t avail = h < kRam ? h : kRam;
  for (uint32_t i = 0; i < avail && emitted < limit; i++) {
    Slot s;
    {
      Lock l;
      uint32_t seq = h - 1 - i;
      if (seq + kRam <= g_head) break;   // overwritten meanwhile
      s = g_ram[seq % kRam];
    }
    if (!matches(s.k, filter)) continue;
    Entry e{s.t, s.up, s.k, s.msg};
    emitted++;
    if (!cb(e)) return;
  }
  if (emitted >= limit) return;

  // Older entries: read the file backwards, newest first, skipping what the RAM tail already covered.
  uint32_t skip, gen;
  size_t pos;
  {
    Lock l;
    if (!g_fs) return;
    File f = LittleFS.open(kPath, "r");
    if (!f) return;
    pos = f.size();
    f.close();
    skip = (g_head - h) + avail;
    gen = g_gen;
  }
  char blk[256];
  char line[200];
  size_t idx = sizeof(line) - 1;
  line[idx] = 0;
  auto emit = [&]() -> int {   // 1 continue, 0 stop
    if (idx == sizeof(line) - 1) return 1;
    char* p = line + idx;
    idx = sizeof(line) - 1;
    if (skip) { skip--; return 1; }
    Entry e;
    if (!parseLine(p, e) || !matches(e.k, filter)) return 1;
    emitted++;
    if (!cb(e)) return 0;
    return emitted < limit ? 1 : 0;
  };
  while (pos > 0) {
    size_t n = pos < sizeof(blk) ? pos : sizeof(blk);
    pos -= n;
    {
      Lock l;
      if (gen != g_gen) return;
      File f = LittleFS.open(kPath, "r");
      if (!f || !f.seek(pos) || f.read((uint8_t*)blk, n) != (int)n) return;
      f.close();
    }
    for (size_t i = n; i-- > 0;) {
      if (blk[i] == '\n') { if (!emit()) return; }
      else if (idx > 0) line[--idx] = blk[i];
    }
  }
  emit();
}

}  // namespace evlog
