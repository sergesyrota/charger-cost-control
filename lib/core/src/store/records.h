// Fixed-size little-endian binary records for LittleFS ring files (PLAN §9.3). Pure. Owner: store.
// Encoding is explicit byte packing (no struct memcpy) so layout is stable across compilers.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace rec {

constexpr int16_t kNoTenths = INT16_MIN;     // "unknown" for *10 fields
int16_t toTenths(float v);                   // NAN -> kNoTenths, clamped
float fromTenths(int16_t v);                 // kNoTenths -> NAN

// /hourly.bin — one per Chicago hour (written at hour end, only with valid clock).
constexpr size_t kHourlySize = 16;
struct Hourly {
  uint32_t hour_epoch = 0;     // UTC epoch of hour start
  int16_t price10 = kNoTenths; // avg supply price ×10 (5-min avg or current)
  int16_t dayahead10 = kNoTenths;
  uint8_t relay_on_min = 0;    // 0..60
  uint16_t kwh100 = 0;         // kWh into car ×100
  uint8_t soc_start = 0xFF;    // 0xFF = unknown
  uint8_t soc_end = 0xFF;
  uint16_t avg_kw100 = 0;      // avg charge power while charging ×100
  uint8_t flags = 0;           // bits 0-2 Mode, bit3 car seen, bit4 charged
};
void encode(const Hourly& r, uint8_t out[kHourlySize]);
bool decode(const uint8_t in[kHourlySize], Hourly& r);  // false if blank/erased (all 0xFF / epoch 0)

// /sessions.bin — one per relay-ON period (written at OFF).
constexpr size_t kSessionSize = 32;
enum SessionFlags : uint8_t { kSessCarDetected = 1, kSessCharged = 2, kSessManual = 4 };
struct Session {
  uint32_t start = 0, end = 0;  // epochs
  uint8_t soc_start = 0xFF, soc_end = 0xFF;
  uint16_t kwh100 = 0;
  int32_t cost_c10 = 0;         // cost in ¢ ×10 (supply only; may be negative)
  int16_t avg_price10 = kNoTenths;
  uint16_t max_kw100 = 0;
  uint8_t flags = 0;
};
void encode(const Session& r, uint8_t out[kSessionSize]);
bool decode(const uint8_t in[kSessionSize], Session& r);

// /detail.bin — every 5 min (30 days).
constexpr size_t kDetailSize = 12;
struct Detail {
  uint32_t epoch = 0;
  int16_t price10 = kNoTenths;
  int16_t watts = 0;            // raw sign
  uint8_t soc = 0xFF;
  uint8_t relay = 0;            // 0/1
  uint8_t car_status = 0;       // CarStatus as uint8
  uint8_t flags = 0;
};
void encode(const Detail& r, uint8_t out[kDetailSize]);
bool decode(const uint8_t in[kDetailSize], Detail& r);

// Compact JSON rows used by the chunked stats endpoints (docs/API.md). Write into buf (no NUL
// required), return bytes written, or 0 if it doesn't fit (caller flushes and retries).
size_t toJsonRow(const Hourly& r, char* buf, size_t cap);
size_t toJsonRow(const Session& r, char* buf, size_t cap);
size_t toJsonRow(const Detail& r, char* buf, size_t cap);

}  // namespace rec
