// STUBS — owner: car. Split into elm_parse.cpp / obd_pids.cpp / car_status.cpp when implementing.
#include <math.h>
#include <string.h>

#include "car/car_status.h"
#include "car/elm_parse.h"
#include "car/obd_pids.h"

namespace obd {

bool responseComplete(const char* raw, size_t len) { return memchr(raw, '>', len) != nullptr; }
ElmResponse parseResponse(const char*, size_t, const char*) { return ElmResponse(); }

const Pid kSoc{"7E4", "228334"};
const Pid kPackVolts{"7E4", "22432D"};
const Pid kPackAmps{"7E7", "2240D4"};
const Pid kCapacity{"7E4", "2241A3"};
const Pid kChargeCount{"7E4", "2243A5"};
const Pid kOdometer{"7E0", "2234B2"};

Pid cellPid(int, char cmd_buf[8]) { cmd_buf[0] = 0; return Pid{"7E7", cmd_buf}; }
float socPct(const ElmResponse&) { return NAN; }
float packVolts(const ElmResponse&) { return NAN; }
float packAmps(const ElmResponse&) { return NAN; }
float capacityAh(const ElmResponse&) { return NAN; }
int32_t chargeCount(const ElmResponse&) { return -1; }
float odometerMi(const ElmResponse&) { return NAN; }
float cellVolts(const ElmResponse&) { return NAN; }

bool isCharging(float, uint16_t, int8_t) { return false; }
CarStatus classify(bool, ElmStatus, float, uint16_t, int8_t) { return CarStatus::Unknown; }

}  // namespace obd
