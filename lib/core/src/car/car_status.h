// Car status classification (PLAN §8.4). Pure. Owner: car.
#pragma once
#include <stdint.h>

#include "car/elm_parse.h"
#include "core_types.h"

namespace obd {

// sign 0 => |watts| >= threshold; +1 => watts >= threshold; -1 => -watts >= threshold. NAN => false.
bool isCharging(float watts, uint16_t threshold_w, int8_t sign);

// not connected -> NotDetected; SOC cmd CommandError -> Asleep; Ok/NoData -> Online, upgraded to
// Charging when isCharging(watts...). Malformed/Timeout while connected -> Asleep.
CarStatus classify(bool connected, ElmStatus soc_status, float watts, uint16_t threshold_w, int8_t sign);

}  // namespace obd
