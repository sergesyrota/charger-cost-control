// STUB — owner: net.
#include "net/clock.h"

#include <Arduino.h>

namespace sysclock {
void begin() {}
bool valid() { return false; }
int64_t now() { return 0; }
bool chicago(ChicagoTime&) { return false; }
uint32_t uptimeS() { return millis() / 1000; }
int64_t syncedAt() { return 0; }
}  // namespace sysclock
