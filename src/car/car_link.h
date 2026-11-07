// NimBLE central to the Veepeak BLE OBD dongle (PLAN §8.1, port of bolt_data.py transport).
// Notify char 0000fff1-..., write char 0000fff2-... (write without response). ELM327 text protocol:
// "<cmd>\r", accumulate notifications until '>' or 10 s. "AT SH <hdr>" only when header changes
// (reset per connection). No init commands (echo on), parser tolerates echo off. Owner: car.
// Blocking; call from the loop task only. Holds netlock for the whole connect..disconnect session.
#pragma once
#include "core_types.h"

namespace carlink {

void begin();                         // NimBLEDevice::init, central only
void setMac(const char* mac);         // "8C:DE:52:DE:B4:A0"

struct PollOpts {
  bool extras = false;                // also capacity / charge count / odometer (once a day)
  uint16_t threshold_w = 500;
  int8_t power_sign = 0;
};

// One poll: connect -> SOC (+V, I when online) [+extras] -> disconnect. Updates `io` in place:
// status always; soc/soc_at/soc_live, volts/amps/watts, last_seen when the car answered;
// last_poll_at/last_poll_up_s always; error text. `now` = epoch or 0 without clock.
// Returns true if the car answered (online/asleep/charging).
bool poll(CarSnapshot& io, const PollOpts& opts, int64_t now);

bool busy();                          // a poll is in progress (thread-safe)

}  // namespace carlink
