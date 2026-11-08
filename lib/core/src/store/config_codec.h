// Config <-> JSON, for the HTTP API (secrets write-only) and for NVS storage. Pure. Owner: store.
// The API JSON shape is specified in docs/API.md ("GET/PUT /api/config").
#pragma once
#include <ArduinoJson.h>

#include <string>

#include "core_types.h"

namespace cfgjson {

// API form. Secrets are NEVER written: emits pushover.token_set / pushover.user_set booleans.
// Wi-Fi fields (ssid, password_set) are not in Config; web_api adds them.
void toApi(const Config& c, JsonObject out);

// Merge a (partial) PUT body into `c`. Absent field = unchanged. Secrets: non-empty string replaces,
// "" / null / absent keeps current, "<name>_clear": true clears. Validates ranges and formats
// (MAC, HH:MM, https:// OTA url); on error returns false, sets `err`, and leaves `c` unchanged.
bool mergeApi(JsonObjectConst in, Config& c, std::string& err);

// Storage form (includes secrets). fromStorage starts from Config{} defaults, so missing/new
// fields get factory values; returns false on parse error (caller then uses defaults + logs).
std::string toStorage(const Config& c);
bool fromStorage(const char* json, size_t len, Config& out);

// Helpers: "HH:MM" <-> minute of day; days list [0..6] (0=Sun) <-> bitmask.
bool parseHHMM(const char* s, uint16_t& minute_of_day);
void formatHHMM(uint16_t minute_of_day, char out[6]);

}  // namespace cfgjson
