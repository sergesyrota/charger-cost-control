// STUB — owner: price.
#include "price/price_math.h"

namespace price {

float round1(float v) { return isnan(v) ? NAN : floorf(v * 10.0f + 0.5f) / 10.0f; }
HourStats currentHour(const FivePoint*, size_t, int64_t) { return HourStats(); }
float currentSupply(const FivePoint*, size_t, const float[24], const ChicagoTime&, bool* estimated) {
  if (estimated) *estimated = false;
  return NAN;
}
float lowestBy7amSupply(const float[24], const float[24], uint8_t) { return NAN; }
float minPrice(float lowest7, float offset) { return isnan(lowest7) ? NAN : round1(lowest7 + offset); }
void derive(PriceSnapshot&, const ChicagoTime&, uint16_t, float) {}

}  // namespace price
