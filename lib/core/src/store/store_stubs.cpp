// STUBS — owner: store. Split into records.cpp / config_codec.cpp when implementing.
#include <math.h>
#include <stdio.h>

#include "store/config_codec.h"
#include "store/records.h"

namespace rec {
int16_t toTenths(float v) { return isnan(v) ? kNoTenths : (int16_t)lroundf(v * 10.0f); }
float fromTenths(int16_t v) { return v == kNoTenths ? NAN : v / 10.0f; }
void encode(const Hourly&, uint8_t[kHourlySize]) {}
bool decode(const uint8_t[kHourlySize], Hourly&) { return false; }
void encode(const Session&, uint8_t[kSessionSize]) {}
bool decode(const uint8_t[kSessionSize], Session&) { return false; }
void encode(const Detail&, uint8_t[kDetailSize]) {}
bool decode(const uint8_t[kDetailSize], Detail&) { return false; }
size_t toJsonRow(const Hourly&, char*, size_t) { return 0; }
size_t toJsonRow(const Session&, char*, size_t) { return 0; }
size_t toJsonRow(const Detail&, char*, size_t) { return 0; }
}  // namespace rec

namespace cfgjson {
void toApi(const Config&, JsonObject) {}
bool mergeApi(JsonObjectConst, Config&, std::string& err) { err = "not implemented"; return false; }
std::string toStorage(const Config&) { return "{}"; }
bool fromStorage(const char*, size_t, Config& out) { out = Config(); return false; }
bool parseHHMM(const char*, uint16_t&) { return false; }
void formatHHMM(uint16_t m, char out[6]) { snprintf(out, 6, "%02u:%02u", (unsigned)(m / 60) % 100, (unsigned)(m % 60)); }
}  // namespace cfgjson
