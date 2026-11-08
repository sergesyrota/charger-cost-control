// Bolt PIDs and formulas (PLAN §8.3, bolt_data.py). Owner: car.
#include "car/obd_pids.h"

#include <math.h>
#include <stdio.h>

namespace obd {

const Pid kSoc{"7E4", "228334"};
const Pid kPackVolts{"7E4", "22432D"};
const Pid kPackAmps{"7E7", "2240D4"};
const Pid kCapacity{"7E4", "2241A3"};
const Pid kChargeCount{"7E4", "2243A5"};
const Pid kOdometer{"7E0", "2234B2"};

Pid cellPid(int n, char cmd_buf[8]) {
  if (n < 1 || n > 96) {
    cmd_buf[0] = 0;
  } else {
    unsigned v = n <= 31 ? 0x4181u + (unsigned)(n - 1) : 0x4200u + (unsigned)(n - 32);
    snprintf(cmd_buf, 8, "22%04X", v);
  }
  return Pid{"7E7", cmd_buf};
}

static bool has(const ElmResponse& r, int data_bytes) {
  return r.status == ElmStatus::Ok && r.len >= 2 + data_bytes;
}
static unsigned ab(const ElmResponse& r) { return r.bytes[2] * 256u + r.bytes[3]; }

float socPct(const ElmResponse& r) { return has(r, 1) ? r.bytes[2] * 100.0f / 255.0f : NAN; }
float packVolts(const ElmResponse& r) { return has(r, 2) ? ab(r) * 0.52f : NAN; }
float packAmps(const ElmResponse& r) {
  if (!has(r, 2)) return NAN;
  return ((int)(int8_t)r.bytes[2] * 256 + r.bytes[3]) / 20.0f;
}
float capacityAh(const ElmResponse& r) { return has(r, 2) ? ab(r) / 10.0f : NAN; }
int32_t chargeCount(const ElmResponse& r) { return has(r, 2) ? (int32_t)ab(r) : -1; }
float odometerMi(const ElmResponse& r) {
  if (!has(r, 4)) return NAN;
  uint32_t v = ((uint32_t)r.bytes[2] << 24) | ((uint32_t)r.bytes[3] << 16) |
               ((uint32_t)r.bytes[4] << 8) | r.bytes[5];
  return (float)(v * 0.009708816);
}
float cellVolts(const ElmResponse& r) { return has(r, 2) ? ab(r) * 5.0f / 65536.0f : NAN; }

}  // namespace obd
