// On-device ComEd fetch loop (PLAN §6.1). Uses https::get + price::FiveMinParser/ServletParser,
// parses into scratch buffers and swaps into the snapshot only on success. Owner: price.
// Cadence: 5-min feed every 5 min aligned to :01,:06,...; day-ahead today hourly (cached per date);
// tomorrow at/after 17:00 Chicago (cached per date); actuals hourly. Requires a valid clock.
#pragma once
#include "core_types.h"

namespace prices {

void begin();

// Call from the loop task. Performs at most ONE due fetch per call (blocking TLS) and returns true
// if the snapshot changed (caller then runs price::derive + the decision). No-op if !clock valid.
bool tick(const ChicagoTime& now);

// Make the 5-min feed due immediately (e.g. after boot/NTP or from the UI).
void forceFiveMin();

// The live snapshot. Only the loop task mutates it; readers on other tasks must go through
// app::withPrices(), which holds the app state lock.
PriceSnapshot& snapshot();

// Last fetch error for diagnostics ("" if ok).
const char* lastError();

}  // namespace prices
