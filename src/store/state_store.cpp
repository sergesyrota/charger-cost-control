// NVS persistence of RuntimeState. Owner: store.
#include "store/state_store.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>

#include <string.h>

#include <string>

namespace statestore {

namespace {
constexpr uint32_t kSocWriteGapMs = 10UL * 60 * 1000;

RuntimeState g_saved;          // what is in NVS
RuntimeState g_latest;         // newest requested (may be ahead of NVS by a SOC-only change)
bool g_dirty = false;          // g_latest differs from g_saved (SOC only)
uint32_t g_last_write_ms = 0;
bool g_written_once = false;

bool socSame(const RuntimeState& a, const RuntimeState& b) {
  bool an = isnan(a.last_soc), bn = isnan(b.last_soc);
  return an == bn && (an || a.last_soc == b.last_soc) && a.last_soc_at == b.last_soc_at;
}
bool restSame(const RuntimeState& a, const RuntimeState& b) {
  return strcmp(a.charging_state, b.charging_state) == 0 && a.relay_cmd == b.relay_cmd && a.ovr.mode == b.ovr.mode &&
         a.ovr.until == b.ovr.until;
}

void write(const RuntimeState& s) {
  JsonDocument d;
  d["cs"] = std::string(s.charging_state);
  if (!isnan(s.last_soc)) d["soc"] = s.last_soc;
  d["soc_at"] = s.last_soc_at;
  d["relay"] = s.relay_cmd;
  d["mode"] = modeName(s.ovr.mode);
  d["until"] = s.ovr.until;
  std::string j;
  serializeJson(d, j);
  Preferences p;
  if (p.begin("cc", false)) {
    p.putBytes("state", j.data(), j.size());
    p.end();
  }
  g_saved = s;
  g_dirty = false;
  g_last_write_ms = millis();
  g_written_once = true;
}
}  // namespace

bool load(RuntimeState& out) {
  out = RuntimeState();
  bool ok = false;
  Preferences p;
  if (p.begin("cc", true)) {
    size_t n = p.getBytesLength("state");
    if (n && n < 512) {
      std::string j(n, '\0');
      p.getBytes("state", &j[0], n);
      JsonDocument d;
      if (!deserializeJson(d, j) && d.is<JsonObject>()) {
        const char* cs = d["cs"] | "";
        if (*cs && strlen(cs) < kStateNameMax) strcpy(out.charging_state, cs);
        if (d["soc"].is<float>()) out.last_soc = d["soc"].as<float>();
        out.last_soc_at = d["soc_at"] | (int64_t)0;
        out.relay_cmd = d["relay"] | false;
        Mode m;
        if (modeFromName(d["mode"] | "", m)) out.ovr.mode = m;
        out.ovr.until = d["until"] | (int64_t)0;
        ok = true;
      }
    }
    p.end();
  }
  g_saved = g_latest = out;
  g_dirty = false;
  return ok;
}

void save(const RuntimeState& s) {
  g_latest = s;
  if (!restSame(s, g_saved)) return write(s);
  if (socSame(s, g_saved)) { g_dirty = false; return; }
  g_dirty = true;
  if (!g_written_once || millis() - g_last_write_ms >= kSocWriteGapMs) write(s);
}

void flush() {
  if (g_dirty) write(g_latest);
}

}  // namespace statestore
