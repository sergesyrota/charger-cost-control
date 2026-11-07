// Port of price-worker/src/prices.ts + the comed.ts hour averaging. Owner: price.
#include "price/price_math.h"

namespace price {

static const int kHourlyAvgMinMinutes = 30;
static const int kRecentPoints = 6;

float round1(float v) { return isnan(v) ? NAN : (float)(floor((double)v * 10.0 + 0.5) / 10.0); }

HourStats currentHour(const FivePoint* pts, size_t n, int64_t hour_start) {
  HourStats r;
  double total = 0;
  uint16_t count = 0;
  for (size_t i = 0; i < n; i++) {
    int64_t e = pts[i].epoch;
    if (e >= hour_start && e < hour_start + 3600) { total += pts[i].price; count++; }
  }
  if (count) r.avg = (float)(total / count);
  r.known_minutes = count * 5;
  return r;
}

float currentSupply(const FivePoint* pts, size_t n, const float da_today[24], const ChicagoTime& now,
                    bool* estimated) {
  if (estimated) *estimated = false;
  if (n > 0) {
    HourStats hs = currentHour(pts, n, now.hourStart());
    if (hs.known_minutes >= kHourlyAvgMinMinutes && !isnan(hs.avg)) return round1(hs.avg);
    // rule 2: mean of the 6 most recent by epoch (insertion into a small sorted array)
    uint32_t top_e[kRecentPoints];
    float top_p[kRecentPoints];
    int k = 0;
    for (size_t i = 0; i < n; i++) {
      int j;
      if (k < kRecentPoints) j = k++;
      else if (pts[i].epoch > top_e[kRecentPoints - 1]) j = kRecentPoints - 1;
      else continue;
      while (j > 0 && top_e[j - 1] < pts[i].epoch) { top_e[j] = top_e[j - 1]; top_p[j] = top_p[j - 1]; j--; }
      top_e[j] = pts[i].epoch;
      top_p[j] = pts[i].price;
    }
    double sum = 0;
    for (int i = 0; i < k; i++) sum += top_p[i];
    return round1((float)(sum / k));
  }
  // rule 3: day-ahead for the current hour (hour-ending index); the reference returns it unrounded
  float fb = da_today[(now.hour + 1) % 24];
  if (!isnan(fb)) {
    if (estimated) *estimated = true;
    return fb;
  }
  return NAN;
}

float lowestBy7amSupply(const float today[24], const float tomorrow[24], uint8_t hour) {
  float best = NAN;
  auto consider = [&](float v) { if (!isnan(v) && (isnan(best) || v < best)) best = v; };
  if (hour < 17) {
    for (int i = 0; i < 8; i++) consider(today[i]);
  } else {
    for (int i = (hour + 1) % 24; i < 24; i++) consider(today[i]);
    for (int i = 0; i < 8; i++) consider(tomorrow[i]);
  }
  return round1(best);
}

float minPrice(float lowest7, float offset) { return isnan(lowest7) ? NAN : round1(lowest7 + offset); }

void derive(PriceSnapshot& s, const ChicagoTime& now, uint16_t stale_min, float min_offset) {
  static const float kNone[24] = {NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN,
                                  NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN, NAN};
  uint32_t today = now.yyyymmdd(), tomorrow = addDays(today, 1);
  const float* t = kNone;
  const float* tm = kNone;
  if (s.da_today_date == today) t = s.da_today;
  else if (s.da_tomorrow_date == today) t = s.da_tomorrow;  // day rolled over since tomorrow was fetched
  if (s.da_tomorrow_date == tomorrow) tm = s.da_tomorrow;

  bool fresh = s.five_fetched_at != 0 && now.epoch - s.five_fetched_at <= (int64_t)stale_min * 60;
  size_t n = fresh ? s.five_count : 0;

  HourStats hs = currentHour(s.five, n, now.hourStart());
  s.hour_avg = round1(hs.avg);
  s.known_minutes = hs.known_minutes;
  s.current = currentSupply(s.five, n, t, now, &s.estimated);
  s.lowest7 = lowestBy7amSupply(t, tm, now.hour);
  s.min_price = minPrice(s.lowest7, min_offset);
}

}  // namespace price
