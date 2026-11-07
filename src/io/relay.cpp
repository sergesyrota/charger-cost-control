// Minimal working implementation (owner: net may refine).
#include "io/relay.h"

#include <Arduino.h>

namespace relay {
static bool s_high = true, s_on = false;
static void drive() { digitalWrite(kPin, (s_on == s_high) ? HIGH : LOW); }
void begin(bool active_high, bool initial_on) {
  s_high = active_high; s_on = initial_on;
  drive();  // set level before switching to output to avoid a glitch
  pinMode(kPin, OUTPUT);
  drive();
}
void set(bool on) { s_on = on; drive(); }
bool get() { return s_on; }
void setPolarity(bool active_high) { s_high = active_high; drive(); }
}  // namespace relay
