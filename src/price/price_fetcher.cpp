// STUB — owner: price.
#include "price/price_fetcher.h"

namespace prices {
static PriceSnapshot s_snap;
void begin() {}
bool tick(const ChicagoTime&) { return false; }
void forceFiveMin() {}
PriceSnapshot& snapshot() { return s_snap; }
const char* lastError() { return ""; }
}  // namespace prices
