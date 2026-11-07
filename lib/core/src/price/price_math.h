// Price math — exact port of price-worker/src/prices.ts (PLAN §6.2, §6.3). Pure.
// Owner: price.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core_types.h"

namespace price {

float round1(float v);  // round to 0.1 like JS Math.round(v*10)/10; NAN stays NAN

struct HourStats {
  float avg = NAN;             // mean of points in the hour, NAN if none
  uint16_t known_minutes = 0;  // 5 * count
};

// Points whose epoch falls in the Chicago hour starting at `hour_start` (ChicagoTime::hourStart()),
// using the same point->hour attribution as comed.ts.
HourStats currentHour(const FivePoint* pts, size_t n, int64_t hour_start);

// currentSupply:
//  1. known_minutes >= 30 -> current-hour average
//  2. else any points -> mean of the 6 most recent (by epoch)
//  3. else da_today[(hour + 1) % 24]   (*estimated = true)
//  4. round1. NAN if nothing available.
// Pass n = 0 when the 5-min feed is stale (rule 3 then applies).
float currentSupply(const FivePoint* pts, size_t n, const float da_today[24], const ChicagoTime& now,
                    bool* estimated);

// lowestBy7amSupply: hour < 17 -> min(today[0..7]); else min(today[(hour+1)%24..23] ∪ tomorrow[0..7]).
// NAN entries ignored; NAN if none; round1. `tomorrow` may be all-NAN.
float lowestBy7amSupply(const float today[24], const float tomorrow[24], uint8_t chicago_hour);

// "min" price for logic: round1(lowest7 + offset); NAN if lowest7 is NAN.
float minPrice(float lowest7, float offset);

// Recompute every derived field of `s` for `now`:
//  - day-ahead arrays only count if their *_date matches today / tomorrow (else treated as all NAN)
//  - 5-min points ignored when five_fetched_at is 0 or older than stale_min minutes
//  - fills current, estimated, hour_avg, known_minutes, lowest7, min_price
void derive(PriceSnapshot& s, const ChicagoTime& now, uint16_t stale_min, float min_offset);

}  // namespace price
