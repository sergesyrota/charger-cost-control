// SNTP clock (PLAN §5). Not persisted. TZ "CST6CDT,M3.2.0,M11.1.0". Owner: net.
// valid() is false until the first sync yields epoch >= kMinValidEpoch. Thread-safe.
#pragma once
#include <stdint.h>

#include "util/chicago_time.h"

namespace sysclock {

void begin();                    // configure SNTP (pool.ntp.org, time.google.com) + TZ; call once Wi-Fi starts
bool valid();
int64_t now();                   // UTC epoch seconds, 0 if !valid()
bool chicago(ChicagoTime& out);  // false if !valid()
uint32_t uptimeS();              // seconds since boot (always available)
int64_t syncedAt();              // epoch of last successful sync, 0 if never

}  // namespace sysclock
