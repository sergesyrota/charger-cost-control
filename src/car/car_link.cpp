// STUB — owner: car.
#include "car/car_link.h"

namespace carlink {
void begin() {}
void setMac(const char*) {}
bool poll(CarSnapshot& io, const PollOpts&, int64_t now) {
  io.status = CarStatus::NotDetected;
  io.last_poll_at = now;
  return false;
}
bool busy() { return false; }
}  // namespace carlink
