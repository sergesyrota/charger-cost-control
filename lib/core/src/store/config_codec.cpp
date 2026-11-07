// Config <-> JSON (API form without secrets, NVS storage form with). Pure. Owner: store.
#include "store/config_codec.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace cfgjson {

bool parseHHMM(const char* s, uint16_t& m) {
  if (!s || !isdigit((unsigned char)*s)) return false;
  int h = *s++ - '0';
  if (isdigit((unsigned char)*s)) h = h * 10 + (*s++ - '0');
  if (*s++ != ':') return false;
  if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) || s[2]) return false;
  int mi = (s[0] - '0') * 10 + (s[1] - '0');
  if (h > 23 || mi > 59) return false;
  m = (uint16_t)(h * 60 + mi);
  return true;
}

void formatHHMM(uint16_t m, char out[6]) { snprintf(out, 6, "%02u:%02u", (unsigned)(m / 60) % 100, (unsigned)(m % 60)); }

static std::string cp(const char* s) { return std::string(s); }   // std::string values are copied by ArduinoJson

void toApi(const Config& c, JsonObject o) {
  o["relay_active_high"] = c.relay_active_high;
  o["default_relay"] = c.default_relay;
  o["stale_min"] = c.stale_min;
  o["full_hold_min"] = c.full_hold_min;
  o["min_offset"] = c.min_offset;
  o["soc_target_enabled"] = c.soc_target_enabled;
  o["soc_target"] = c.soc_target;
  JsonArray sch = o["schedule"].to<JsonArray>();
  for (uint8_t i = 0; i < c.window_count && i < kMaxWindows; i++) {
    const ScheduleWindow& w = c.windows[i];
    JsonObject jw = sch.add<JsonObject>();
    JsonArray d = jw["days"].to<JsonArray>();
    for (int b = 0; b < 7; b++)
      if (w.days & (1 << b)) d.add(b);
    char t[6];
    formatHHMM(w.start_min, t);
    jw["start"] = cp(t);
    formatHHMM(w.end_min, t);
    jw["end"] = cp(t);
  }
  o["dongle_mac"] = cp(c.dongle_mac);
  o["poll_on_s"] = c.poll_on_s;
  o["poll_off_min"] = c.poll_off_min;
  o["charge_threshold_w"] = c.charge_threshold_w;
  o["power_sign"] = c.power_sign;
  o["probe_delay_s"] = c.probe_delay_s;
  JsonObject p = o["pushover"].to<JsonObject>();
  p["enabled"] = c.pushover_enabled;
  p["device"] = cp(c.pushover_device);
  p["token_set"] = c.pushover_token[0] != 0;   // secrets: only presence is ever exposed
  p["user_set"] = c.pushover_user[0] != 0;
  o["wifi"].to<JsonObject>()["connect_timeout_s"] = c.wifi_timeout_s;
  o["ota_url"] = cp(c.ota_url);
}

namespace {

struct Ctx {
  JsonObjectConst in;
  std::string& err;
  bool bad = false;
  void fail(const char* key, const char* what) {
    if (!bad) { err = std::string(key) + ": " + what; bad = true; }
  }
  bool has(JsonObjectConst o, const char* k) { return !o[k].isNull(); }
  void flag(JsonObjectConst o, const char* k, bool& dst) {
    if (!has(o, k)) return;
    if (!o[k].is<bool>()) return fail(k, "must be true/false");
    dst = o[k].as<bool>();
  }
  // number in [lo,hi] -> integer/float destination
  template <class T>
  void num(JsonObjectConst o, const char* k, double lo, double hi, T& dst) {
    if (!has(o, k)) return;
    if (!o[k].is<double>()) return fail(k, "must be a number");
    double v = o[k].as<double>();
    if (v < lo || v > hi) return fail(k, "out of range");
    dst = (T)v;
  }
  // string with max length (excl. NUL) -> char buffer; returns true if provided
  bool str(JsonObjectConst o, const char* k, char* dst, size_t cap) {
    if (!has(o, k)) return false;
    if (!o[k].is<const char*>()) { fail(k, "must be a string"); return false; }
    const char* s = o[k].as<const char*>();
    if (strlen(s) >= cap) { fail(k, "too long"); return false; }
    strcpy(dst, s);
    return true;
  }
};

bool validMac(char* m) {
  if (strlen(m) != 17) return false;
  for (int i = 0; i < 17; i++) {
    if (i % 3 == 2) { if (m[i] != ':') return false; }
    else if (!isxdigit((unsigned char)m[i])) return false;
    else m[i] = (char)toupper((unsigned char)m[i]);
  }
  return true;
}

}  // namespace

bool mergeApi(JsonObjectConst in, Config& c, std::string& err) {
  Config t = c;
  Ctx x{in, err};
  x.flag(in, "relay_active_high", t.relay_active_high);
  x.flag(in, "default_relay", t.default_relay);
  x.num(in, "stale_min", 5, 120, t.stale_min);
  x.num(in, "full_hold_min", 0, 1440, t.full_hold_min);
  x.num(in, "min_offset", -10, 10, t.min_offset);
  x.flag(in, "soc_target_enabled", t.soc_target_enabled);
  x.num(in, "soc_target", 0, 100, t.soc_target);
  x.num(in, "poll_on_s", 10, 3600, t.poll_on_s);
  x.num(in, "poll_off_min", 5, 1440, t.poll_off_min);
  x.num(in, "charge_threshold_w", 0, 20000, t.charge_threshold_w);
  x.num(in, "power_sign", -1, 1, t.power_sign);
  x.num(in, "probe_delay_s", 0, 600, t.probe_delay_s);

  if (x.str(in, "dongle_mac", t.dongle_mac, sizeof(t.dongle_mac)) && !validMac(t.dongle_mac))
    x.fail("dongle_mac", "must look like XX:XX:XX:XX:XX:XX");

  if (x.str(in, "ota_url", t.ota_url, sizeof(t.ota_url))) {
    if (!t.ota_url[0]) strcpy(t.ota_url, kDefaultOtaUrl);
    else if (strncmp(t.ota_url, "https://", 8) != 0 || !t.ota_url[8]) x.fail("ota_url", "must start with https://");
  }

  if (!in["schedule"].isNull()) {
    if (!in["schedule"].is<JsonArrayConst>()) x.fail("schedule", "must be a list");
    else {
      JsonArrayConst a = in["schedule"].as<JsonArrayConst>();
      if (a.size() > kMaxWindows) x.fail("schedule", "too many windows (max 8)");
      else {
        t.window_count = 0;
        for (JsonObjectConst w : a) {
          ScheduleWindow sw;
          if (!w["days"].is<JsonArrayConst>()) { x.fail("schedule", "window needs a days list"); break; }
          for (JsonVariantConst d : w["days"].as<JsonArrayConst>()) {
            if (!d.is<int>() || d.as<int>() < 0 || d.as<int>() > 6) { x.fail("schedule", "days must be 0..6"); break; }
            sw.days |= (uint8_t)(1 << d.as<int>());
          }
          if (!parseHHMM(w["start"] | "", sw.start_min) || !parseHHMM(w["end"] | "", sw.end_min))
            x.fail("schedule", "times must be HH:MM");
          if (x.bad) break;
          t.windows[t.window_count++] = sw;
        }
      }
    }
  }

  if (!in["pushover"].isNull()) {
    if (!in["pushover"].is<JsonObjectConst>()) x.fail("pushover", "must be an object");
    else {
      JsonObjectConst p = in["pushover"].as<JsonObjectConst>();
      x.flag(p, "enabled", t.pushover_enabled);
      x.str(p, "device", t.pushover_device, sizeof(t.pushover_device));
      // write-only secrets: empty / null / absent keeps; *_clear erases
      char tmp[40];
      if (x.str(p, "token", tmp, sizeof(tmp)) && tmp[0]) strcpy(t.pushover_token, tmp);
      if (x.str(p, "user", tmp, sizeof(tmp)) && tmp[0]) strcpy(t.pushover_user, tmp);
      if (p["token_clear"] | false) t.pushover_token[0] = 0;
      if (p["user_clear"] | false) t.pushover_user[0] = 0;
    }
  }
  if (!in["wifi"].isNull()) {
    if (!in["wifi"].is<JsonObjectConst>()) x.fail("wifi", "must be an object");
    else x.num(in["wifi"].as<JsonObjectConst>(), "connect_timeout_s", 10, 3600, t.wifi_timeout_s);
  }

  if (x.bad) return false;
  c = t;
  return true;
}

std::string toStorage(const Config& c) {
  JsonDocument d;
  toApi(c, d.to<JsonObject>());
  d["pushover"]["token"] = cp(c.pushover_token);
  d["pushover"]["user"] = cp(c.pushover_user);
  std::string s;
  serializeJson(d, s);
  return s;
}

bool fromStorage(const char* json, size_t len, Config& out) {
  JsonDocument d;
  if (deserializeJson(d, json, len) || !d.is<JsonObject>()) return false;
  Config t;
  std::string e;
  if (!mergeApi(d.as<JsonObjectConst>(), t, e)) return false;
  out = t;
  return true;
}

}  // namespace cfgjson
