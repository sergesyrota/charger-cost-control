// Car status classification (PLAN §8.4). Owner: car.
#include "car/car_status.h"

#include <math.h>

namespace obd {

bool isCharging(float watts, uint16_t threshold_w, int8_t sign) {
  if (isnan(watts)) return false;
  float w = sign > 0 ? watts : sign < 0 ? -watts : fabsf(watts);
  return w >= (float)threshold_w;
}

CarStatus classify(bool connected, ElmStatus soc_status, float watts, uint16_t threshold_w, int8_t sign) {
  if (!connected) return CarStatus::NotDetected;
  if (soc_status != ElmStatus::Ok && soc_status != ElmStatus::NoData) return CarStatus::Asleep;
  return isCharging(watts, threshold_w, sign) ? CarStatus::Charging : CarStatus::Online;
}

}  // namespace obd
