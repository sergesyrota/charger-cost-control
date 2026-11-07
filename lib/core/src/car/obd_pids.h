// Bolt PIDs and formulas (PLAN §8.3, bolt_data.py). Pure. Owner: car.
// All decoders return NAN (or -1 for ints) unless r.status == Ok and enough bytes are present.
#pragma once
#include <stdint.h>

#include "car/elm_parse.h"

namespace obd {

struct Pid {
  const char* header;  // "7E4" (sent as "AT SH 7E4" only when it changes)
  const char* cmd;     // "228334"
};

extern const Pid kSoc;          // 7E4 228334  A*100/255
extern const Pid kPackVolts;    // 7E4 22432D  (A*256+B)*0.52
extern const Pid kPackAmps;     // 7E7 2240D4  (int8(A)*256+B)/20
extern const Pid kCapacity;     // 7E4 2241A3  (A*256+B)/10 Ah
extern const Pid kChargeCount;  // 7E4 2243A5  A*256+B
extern const Pid kOdometer;     // 7E0 2234B2  (A<<24|B<<16|C<<8|D)*0.009708816 mi

// Cell n (1..96): 7E7, "22" + hex(0x4181+n-1) for n<=31, hex(0x4200+n-32) for n>=32.
// Writes the command into cmd_buf (>= 8 bytes) and returns a Pid pointing at it.
Pid cellPid(int n, char cmd_buf[8]);

float socPct(const ElmResponse& r);
float packVolts(const ElmResponse& r);
float packAmps(const ElmResponse& r);
float capacityAh(const ElmResponse& r);
inline float capacityKwh(float ah) { return ah * 0.32f; }
int32_t chargeCount(const ElmResponse& r);
float odometerMi(const ElmResponse& r);
float cellVolts(const ElmResponse& r);  // (A*256+B)*5/65536

}  // namespace obd
