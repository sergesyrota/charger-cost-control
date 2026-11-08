// Chicago wall-clock time, passed INTO pure code (pure code never reads the clock).
// Owner: integrator. Implemented (small, shared by everyone).
#pragma once
#include <stdint.h>

constexpr int64_t kMinValidEpoch = 1704067200;  // 2024-01-01T00:00Z; below this the clock is "not valid"

struct ChicagoTime {
  int64_t epoch = 0;          // UTC seconds
  int32_t utc_offset_s = 0;   // -21600 (CST) or -18000 (CDT)
  int16_t year = 0;
  uint8_t month = 0;          // 1..12
  uint8_t day = 0;            // 1..31
  uint8_t hour = 0;           // 0..23
  uint8_t minute = 0;         // 0..59
  uint8_t second = 0;         // 0..59
  uint8_t wday = 0;           // 0 = Sunday .. 6 = Saturday

  uint32_t yyyymmdd() const { return (uint32_t)year * 10000u + month * 100u + day; }
  int64_t hourStart() const { return epoch - minute * 60 - second; }   // epoch of HH:00:00 Chicago
  int64_t dayStart() const { return hourStart() - (int64_t)hour * 3600; }  // epoch of local midnight (DST-day edge ignored)
  uint16_t minuteOfDay() const { return (uint16_t)(hour * 60 + minute); }
};

// America/Chicago UTC offset for a UTC epoch (US rules since 2007: CDT from 2nd Sun Mar 02:00 local
// to 1st Sun Nov 02:00 local). Pure; matches POSIX TZ "CST6CDT,M3.2.0,M11.1.0".
int32_t chicagoOffset(int64_t epoch);

// Fill all calendar fields for `epoch` using chicagoOffset().
ChicagoTime chicagoFromEpoch(int64_t epoch);

// Calendar-date arithmetic on YYYYMMDD integers (e.g. tomorrow = addDays(today, 1)).
uint32_t addDays(uint32_t yyyymmdd, int days);
