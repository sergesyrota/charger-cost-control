// STUB — owner: store. Serial-only for now.
#include "store/event_log.h"

#include <Arduino.h>
#include <stdarg.h>

namespace evlog {
const char* kindName(Kind k) {
  static const char* const n[] = {"sys", "relay", "state", "push", "car", "price", "err"};
  return n[(int)k];
}
void begin() {}
void add(Kind k, const char* fmt, ...) {
  char buf[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.printf("[%s] %s\n", kindName(k), buf);
}
void forEach(size_t, const char*, const std::function<bool(const Entry&)>&) {}
}  // namespace evlog
