// Stats + history on LittleFS (PLAN §9.3): hourly / sessions / detail ring files using rec:: codecs.
// Accumulates in RAM, flushes hourly (and on flush()). Records only with a valid clock.
// kWh = ∫ |power| dt from car polls while relay ON; cost = kWh × supply price. Owner: store.
// Readers (fill*/daily/monthly/session getters) are thread-safe and called from web handlers.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core_types.h"
#include "store/records.h"

namespace stats {

void begin();   // after LittleFS mount

// Loop task, about once a minute (and after each car poll / relay change). Handles hour rollover,
// 5-min detail records, session start/end on relay transitions.
void sample(const ChicagoTime* now /*nullptr if no clock*/, const PriceSnapshot& p, const CarSnapshot& car,
            bool relay, Mode mode);
void flush();   // write pending hour/session (before reboot/OTA)

// Sessions. `current` valid while relay ON (kwh/cost so far); `last` = most recent finished.
bool currentSession(rec::Session& out);
bool lastSession(rec::Session& out);

// Chunked JSON writers for AsyncWebServer chunked responses. Each call writes as many comma-joined
// rows (see docs/API.md) as fit into buf and advances `cursor` (opaque, start at 0). Returns bytes
// written; 0 = finished. The caller adds the surrounding "[" "]".
size_t fillHourly(uint32_t from, uint32_t to, uint32_t& cursor, char* buf, size_t cap);
size_t fillDetail(uint32_t from, uint32_t to, uint32_t& cursor, char* buf, size_t cap);
size_t fillSessions(uint16_t limit, uint32_t& cursor, char* buf, size_t cap);   // newest first

// Aggregates (small, computed from hourly records + current RAM accumulators). Newest last.
struct Agg {
  uint32_t key;        // YYYYMMDD (daily) or YYYYMM (monthly), Chicago
  float kwh;
  float cost_c;        // ¢
  uint16_t relay_h10;  // relay-on hours ×10
};
size_t daily(uint16_t days, Agg* out, size_t max);
size_t monthly(uint16_t months, Agg* out, size_t max);

struct FsInfo { size_t total, used; };
FsInfo fsInfo();

}  // namespace stats
