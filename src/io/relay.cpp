// Relay on GPIO 4. Owner: net.
#include "io/relay.h"

#include <Arduino.h>
#include <driver/gpio.h>

namespace relay {

static volatile bool s_high = true, s_on = false;

static void drive() { gpio_set_level((gpio_num_t)kPin, (s_on == s_high) ? 1 : 0); }

void begin(bool active_high, bool initial_on) {
  s_high = active_high;
  s_on = initial_on;
  drive();                // latch the output register first so the pin never shows the wrong level
  pinMode(kPin, OUTPUT);  // registers the pin with the Arduino peripheral manager
  drive();
}

void set(bool on) {
  s_on = on;
  drive();
}

bool get() { return s_on; }

void setPolarity(bool active_high) {
  s_high = active_high;
  drive();
}

}  // namespace relay
