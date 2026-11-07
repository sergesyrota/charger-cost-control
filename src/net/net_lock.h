// The single "network-heavy" lock (CLAUDE.md: never two TLS sessions, never TLS + active BLE GATT).
// Recursive FreeRTOS mutex: https_client takes it internally per request; car_link holds it for a
// whole GATT session; ota holds it across check+download (nested https calls are fine).
// Never take it from the AsyncTCP (web handler) task. Owner: net.
#pragma once
#include <stdint.h>

namespace netlock {

void init();                                      // call once, early in setup()
bool take(const char* who, uint32_t timeout_ms);  // false on timeout
void give();
const char* holder();                             // "" if free (diagnostics)

class Guard {
 public:
  Guard(const char* who, uint32_t timeout_ms = 60000) : ok_(take(who, timeout_ms)) {}
  ~Guard() { if (ok_) give(); }
  bool ok() const { return ok_; }
  Guard(const Guard&) = delete;
  Guard& operator=(const Guard&) = delete;

 private:
  bool ok_;
};

}  // namespace netlock
