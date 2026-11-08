// Event log: /events.log on LittleFS, last ~500 lines (PLAN §9.3). Owner: store.
// Each entry: epoch (0 if no clock) + uptime seconds + kind + UTF-8 message (<= 255 bytes, Russian).
// Also echoed to Serial. Thread-safe (add() may be called from any task).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace evlog {

enum class Kind : uint8_t { Sys, Relay, State, Push, Car, Price, Err };
const char* kindName(Kind k);   // "sys" "relay" "state" "push" "car" "price" "err"

void begin();                   // after LittleFS mount
void add(Kind k, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

struct Entry {
  int64_t t;      // epoch or 0
  uint32_t up;    // uptime s
  Kind k;
  const char* msg;
};
// Newest first; stops after `limit` entries or when cb returns false. kind_filter nullptr = all.
// Safe to call from the web task; must not hold a lock across cb for long (copy lines first).
void forEach(size_t limit, const char* kind_filter, const std::function<bool(const Entry&)>& cb);

}  // namespace evlog
