// Relay on GPIO 4 (signal only). Active-high by default (HIGH = charger ON, verified on bench).
// begin() must be the FIRST thing in setup(): drives the pin to the default level immediately.
// Owner: net.
#pragma once

namespace relay {

constexpr int kPin = 4;

void begin(bool active_high, bool initial_on);
void set(bool on);                      // idempotent
bool get();                             // commanded state (true = charger enabled)
void setPolarity(bool active_high);     // re-drives the pin for the current state

}  // namespace relay
