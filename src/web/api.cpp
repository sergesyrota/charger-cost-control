// JSON API (docs/API.md). Owner: web_api.
// Runs on the AsyncTCP task: no blocking, no TLS/BLE. Mutations go through app:: only.
// Secrets are never serialized: only *_set booleans (cfgjson::toApi + wifi::passwordSet).
#include "web/api.h"

#include <ArduinoJson.h>
#include <esp_heap_caps.h>

#include <memory>
#include <string>
#include <vector>

#include "app/app.h"
#include "net/clock.h"
#include "net/wifi_mgr.h"
#include "ota/updater.h"
#include "store/config_codec.h"
#include "store/event_log.h"
#include "store/stats.h"

namespace api {
namespace {

constexpr size_t kSmallBody = 1024;
constexpr size_t kConfigBody = 4096;
constexpr size_t kLogicBody = 16384;
constexpr size_t kLogMaxBytes = 24000;  // cap RAM used by one /api/log response

// ---------------------------------------------------------------- response helpers
void sendJson(AsyncWebServerRequest* r, int code, const JsonDocument& d) {
  AsyncResponseStream* s = r->beginResponseStream("application/json");
  s->setCode(code);
  s->addHeader("Cache-Control", "no-store");
  serializeJson(d, *s);
  r->send(s);
}

void sendRaw(AsyncWebServerRequest* r, int code, const char* json) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", json);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

void sendErr(AsyncWebServerRequest* r, int code, const char* msg) {
  JsonDocument d;
  d["error"] = msg;
  sendJson(r, code, d);
}

// 202 with the job's sequence number: the UI treats a finished job with seq >= this as fresh.
void sendQueued(AsyncWebServerRequest* r, uint16_t seq) {
  char b[40];
  snprintf(b, sizeof(b), "{\"queued\":true,\"seq\":%u}", (unsigned)seq);
  sendRaw(r, 202, b);
}

// CSRF guard (no auth by owner decision): state-changing routes need a JSON content type (a
// cross-site page can't send one without a CORS preflight, which we never answer) and, when the
// browser sends Origin, it must name this host.
bool sameOriginJson(AsyncWebServerRequest* r) {
  if (!r->contentType().startsWith("application/json")) return false;
  if (!r->hasHeader("Origin")) return true;
  String o = r->header("Origin"), host = r->host();
  int p = o.indexOf("://");
  if (p >= 0) o = o.substring(p + 3);
  return host.length() && o.equalsIgnoreCase(host);
}

// ---------------------------------------------------------------- JSON body routes
using JsonFn = std::function<void(AsyncWebServerRequest*, JsonObject)>;

void onJson(AsyncWebServer& s, const char* uri, WebRequestMethod m, size_t maxBody, JsonFn fn) {
  s.on(
      uri, m,
      [maxBody, fn](AsyncWebServerRequest* r) {
        if (!sameOriginJson(r)) return sendErr(r, 415, "Content-Type: application/json from this device's own page required");
        size_t len = r->contentLength();
        if (len > maxBody) return sendErr(r, 413, "body too large");
        JsonDocument doc;
        if (len > 0) {
          char* body = (char*)r->_tempObject;
          if (!body) return sendErr(r, 400, "empty body");
          if (deserializeJson(doc, body, len) || !doc.is<JsonObject>()) return sendErr(r, 400, "invalid JSON object");
        } else {
          doc.to<JsonObject>();
        }
        fn(r, doc.as<JsonObject>());
      },
      nullptr,
      [maxBody](AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
        if (total > maxBody || total == 0) return;
        if (index == 0) r->_tempObject = malloc(total + 1);  // freed by the request destructor
        char* b = (char*)r->_tempObject;
        if (!b) return;
        memcpy(b + index, data, len);
        if (index + len >= total) b[total] = 0;
      });
}

// ---------------------------------------------------------------- chunked JSON arrays
struct Chunker {
  uint32_t cursor = 0;
  bool first = true, done = false;
};
using FillFn = std::function<size_t(uint32_t&, char*, size_t)>;

void sendChunked(AsyncWebServerRequest* r, FillFn fill) {
  auto st = std::make_shared<Chunker>();
  AsyncWebServerResponse* res =
      r->beginChunkedResponse("application/json", [st, fill](uint8_t* buf, size_t maxLen, size_t) -> size_t {
        if (st->done) return 0;
        if (maxLen < 400) return RESPONSE_TRY_AGAIN;  // must fit the largest row (session ~200 B)
        // The fillers keep their own "emitted" flag in the cursor and prefix ',' when continuing,
        // so only the opening '[' is added here.
        const size_t off = st->first ? 1 : 0;
        size_t n = fill(st->cursor, (char*)buf + off, maxLen - 1 - off);
        if (n > 0) {
          if (st->first) buf[0] = '[';
          st->first = false;
          return n + off;
        }
        st->done = true;
        if (st->first) {
          memcpy(buf, "[]", 2);
          return 2;
        }
        buf[0] = ']';
        return 1;
      });
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}

// ---------------------------------------------------------------- misc helpers
void setEpoch(JsonVariant v, int64_t e) {
  // integer, never double: ArduinoJson stores exactly-float-representable doubles as float and
  // prints them with 6 significant digits (1794085888 -> 1.794086e9).
  if (e > 0) v.set((int64_t)e); else v.set(nullptr);
}
void setTenths(JsonVariant v, int16_t t) {
  float f = rec::fromTenths(t);
  if (isnan(f)) v.set(nullptr); else v.set(f);
}
void setSoc(JsonVariant v, uint8_t s) {
  if (s == 0xFF) v.set(nullptr); else v.set((int)s);
}
void setStr(JsonVariant v, const String& s) {
  if (s.length()) v.set(s.c_str()); else v.set(nullptr);
}

bool queryU32(AsyncWebServerRequest* r, const char* name, uint32_t& out) {
  if (!r->hasParam(name)) return false;
  out = (uint32_t)strtoul(r->getParam(name)->value().c_str(), nullptr, 10);
  return true;
}

bool jobBusy(const app::Job& j) { return j.state == app::JobState::Queued || j.state == app::JobState::Running; }

void jobJson(JsonObject o, const app::Job& j) {
  o["state"] = app::jobStateName(j.state);
  o["error"] = (const char*)j.error;
  setEpoch(o["at"], j.finished_at);
  o["seq"] = j.seq;
}

// Index of the schedule window active at `t`, or -1.
int activeWindow(const Config& c, const ChicagoTime& t) {
  uint16_t m = t.minuteOfDay();
  uint8_t today = 1u << t.wday, yest = 1u << ((t.wday + 6) % 7);
  for (int i = 0; i < c.window_count; i++) {
    const ScheduleWindow& w = c.windows[i];
    if (w.start_min == w.end_min) continue;
    bool in;
    if (w.start_min < w.end_min) in = (w.days & today) && m >= w.start_min && m < w.end_min;
    else in = ((w.days & today) && m >= w.start_min) || ((w.days & yest) && m < w.end_min);
    if (in) return i;
  }
  return -1;
}

// ---------------------------------------------------------------- /api/status
void handleStatus(AsyncWebServerRequest* r) {
  Config cfg = app::config();
  Override ovr = app::getOverride();
  Decision dec = app::decision();
  CarSnapshot car = app::car();
  RuntimeState rt = app::runtime();
  app::Jobs jobs = app::jobs();
  bool relay = app::relayOn();
  bool clk = app::clockValid();
  int64_t now = clk ? sysclock::now() : 0;

  struct {
    float current, hour_avg, lowest7, min_price;
    bool estimated;
    uint16_t known_minutes;
    int64_t five_at;
  } p = {};
  app::withPrices([&](const PriceSnapshot& s) {
    p.current = s.current; p.hour_avg = s.hour_avg; p.lowest7 = s.lowest7; p.min_price = s.min_price;
    p.estimated = s.estimated; p.known_minutes = s.known_minutes; p.five_at = s.five_fetched_at;
  });

  JsonDocument d;
  setEpoch(d["now"], now);
  d["clock_valid"] = clk;
  d["tz_offset_s"] = clk ? chicagoOffset(now) : -21600;
  d["uptime_s"] = sysclock::uptimeS();
  d["reset_reason"] = app::resetReason();

  d["relay"] = relay;
  d["relay_default"] = cfg.default_relay;
  d["mode"] = modeName(ovr.mode);
  setEpoch(d["until"], ovr.until);
  d["soc_target"]["enabled"] = cfg.soc_target_enabled;
  d["soc_target"]["value"] = cfg.soc_target;

  JsonObject jd = d["decision"].to<JsonObject>();
  jd["src"] = decisionSrcName(dec.src);
  jd["reason"] = (const char*)dec.reason;
  jd["state"] = (const char*)dec.state;
  jd["how"] = stateHowName(dec.how);
  jd["block"] = dec.block_index;
  jd["fallback"] = dec.logic_fallback;
  jd["price"] = dec.price;
  jd["price_on"] = dec.price_on;
  jd["price_off"] = dec.price_off;
  jd["known_minutes"] = dec.known_minutes;
  setEpoch(jd["at"], dec.at);
  d["sticky_state"] = (const char*)rt.charging_state;

  ChicagoTime ct;
  int aw = clk && sysclock::chicago(ct) ? activeWindow(cfg, ct) : -1;
  if (aw >= 0) d["schedule_active"] = aw; else d["schedule_active"] = nullptr;

  JsonObject jp = d["price"].to<JsonObject>();
  jp["current"] = p.current;
  jp["estimated"] = p.estimated;
  jp["known"] = !isnan(p.current);
  if (p.five_at > 0 && now > 0) jp["age_s"] = (int64_t)(now - p.five_at); else jp["age_s"] = nullptr;
  jp["known_minutes"] = p.known_minutes;
  jp["hour_avg"] = p.hour_avg;
  jp["lowest7"] = p.lowest7;
  jp["min_price"] = p.min_price;
  jp["min_offset"] = cfg.min_offset;
  jp["stale_min"] = cfg.stale_min;
  char perr[48];
  app::priceError(perr, sizeof(perr));
  jp["last_error"] = perr;

  JsonObject jc = d["car"].to<JsonObject>();
  jc["status"] = carStatusName(car.status);
  jc["soc"] = car.soc;
  jc["soc_live"] = car.soc_live;
  setEpoch(jc["soc_at"], car.soc_at);
  setEpoch(jc["last_seen"], car.last_seen);
  setEpoch(jc["last_poll_at"], car.last_poll_at);
  if (car.last_poll_up_s > 0) jc["last_poll_up_s"] = car.last_poll_up_s; else jc["last_poll_up_s"] = nullptr;
  bool live = car.status == CarStatus::Online || car.status == CarStatus::Charging;
  if (live && !isnan(car.watts)) jc["kw"] = fabsf(car.watts) / 1000.0f; else jc["kw"] = nullptr;
  jc["volts"] = car.volts;
  jc["amps"] = car.amps;
  jc["watts"] = car.watts;
  jc["threshold_w"] = cfg.charge_threshold_w;
  jc["polling"] = jobBusy(jobs.car_poll);
  jc["error"] = (const char*)car.error;

  rec::Session s;
  if (stats::currentSession(s)) {
    JsonObject o = d["session"].to<JsonObject>();
    o["start"] = s.start;
    o["kwh"] = s.kwh100 / 100.0f;
    o["cost_c"] = s.cost_c10 / 10.0f;
    setSoc(o["soc_start"], s.soc_start);
    setTenths(o["avg_price"], s.avg_price10);
  } else {
    d["session"] = nullptr;
  }
  if (stats::lastSession(s)) {
    JsonObject o = d["last_session"].to<JsonObject>();
    o["start"] = s.start;
    o["end"] = s.end;
    o["kwh"] = s.kwh100 / 100.0f;
    o["cost_c"] = s.cost_c10 / 10.0f;
    setSoc(o["soc_start"], s.soc_start);
    setSoc(o["soc_end"], s.soc_end);
  } else {
    d["last_session"] = nullptr;
  }

  JsonObject jw = d["wifi"].to<JsonObject>();
  jw["ssid"] = wifi::ssid();
  jw["sta"] = wifi::staConnected();
  if (wifi::staConnected()) jw["rssi"] = wifi::rssi(); else jw["rssi"] = nullptr;
  setStr(jw["ip"], wifi::ip());
  jw["ap"] = wifi::apActive();
  jw["ap_ssid"] = wifi::apSsid();
  setStr(jw["ap_ip"], wifi::apIp());
  jw["ap_clients"] = wifi::apClients();
  jw["hostname"] = "charger.local";

  JsonObject jh = d["heap"].to<JsonObject>();
  jh["free"] = ESP.getFreeHeap();
  jh["min"] = ESP.getMinFreeHeap();
  jh["largest"] = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  stats::FsInfo fs = stats::fsInfo();
  d["fs"]["total"] = (uint32_t)fs.total;
  d["fs"]["used"] = (uint32_t)fs.used;
  JsonObject jf = d["fw"].to<JsonObject>();
  jf["version"] = FW_VERSION;
  jf["rollback"] = ota::runningFromRollback();
  jf["pending_verify"] = ota::pendingVerify();

  JsonObject jb = d["banners"].to<JsonObject>();
  jb["relay_on_not_charging"] = relay && car.status != CarStatus::Charging;
  jb["no_clock"] = !clk;
  jb["price_stale"] = clk && isnan(p.current);
  jb["ap_mode"] = wifi::apActive();
  jb["rollback"] = ota::runningFromRollback();

  JsonObject jj = d["jobs"].to<JsonObject>();
  jobJson(jj["car_poll"].to<JsonObject>(), jobs.car_poll);
  jobJson(jj["push_test"].to<JsonObject>(), jobs.push_test);
  jobJson(jj["ota"].to<JsonObject>(), jobs.ota);
  jobJson(jj["wifi"].to<JsonObject>(), jobs.wifi);

  sendJson(r, 200, d);
}

// ---------------------------------------------------------------- /api/prices
void appendF(String& s, float v) {
  if (isnan(v)) { s += "null"; return; }
  char b[16];
  snprintf(b, sizeof(b), "%.1f", v);
  s += b;
}
void appendArr(String& s, const float* a) {
  s += '[';
  for (int i = 0; i < 24; i++) {
    if (i) s += ',';
    appendF(s, a[i]);
  }
  s += ']';
}
void appendEpoch(String& s, int64_t e) {
  if (e > 0) s += (unsigned long)e; else s += "null";
}

void handlePrices(AsyncWebServerRequest* r) {
  ChicagoTime ct;
  bool clk = app::clockValid() && sysclock::chicago(ct);
  String s;
  s.reserve(7000);
  s += "{\"date\":";
  if (clk) { s += ct.yyyymmdd(); s += ",\"hour\":"; s += ct.hour; s += ",\"minute\":"; s += ct.minute; }
  else s += "null,\"hour\":null,\"minute\":null";
  app::withPrices([&](const PriceSnapshot& p) {
    s += ",\"day_ahead_today\":";
    appendArr(s, p.da_today);
    s += ",\"day_ahead_tomorrow\":";
    if (p.da_tomorrow_date) appendArr(s, p.da_tomorrow); else s += "null";
    s += ",\"tomorrow_date\":";
    if (p.da_tomorrow_date) s += p.da_tomorrow_date;
    else if (clk) s += addDays(ct.yyyymmdd(), 1);
    else s += "null";
    s += ",\"actual_today\":";
    appendArr(s, p.actual_today);
    s += ",\"five_min\":[";
    int64_t day0 = clk ? ct.dayStart() : 0;
    bool first = true;
    for (int i = (int)p.five_count - 1; i >= 0; i--) {  // snapshot is newest first -> emit oldest first
      if (p.five[i].epoch < day0) continue;
      if (!first) s += ',';
      first = false;
      s += '[';
      s += p.five[i].epoch;
      s += ',';
      appendF(s, p.five[i].price);
      s += ']';
    }
    s += "],\"fetched\":{\"five_min\":";
    appendEpoch(s, p.five_fetched_at);
    s += ",\"day_ahead\":";
    appendEpoch(s, p.da_fetched_at);
    s += ",\"actual\":";
    appendEpoch(s, p.actual_fetched_at);
    s += "}}";
  });
  sendRaw(r, 200, s.c_str());
}

// ---------------------------------------------------------------- /api/config
void configBody(JsonDocument& d) {
  Config c = app::config();
  JsonObject o = d.to<JsonObject>();
  cfgjson::toApi(c, o);
  JsonObject w = o["wifi"].is<JsonObject>() ? o["wifi"].as<JsonObject>() : o["wifi"].to<JsonObject>();
  w["ssid"] = wifi::ssid();
  w["password_set"] = wifi::passwordSet();
  w["connect_timeout_s"] = c.wifi_timeout_s;
}

void sendConfig(AsyncWebServerRequest* r) {
  JsonDocument d;
  configBody(d);
  sendJson(r, 200, d);
}

void putConfig(AsyncWebServerRequest* r, JsonObject body) {
  if (body["wifi"].is<JsonObject>()) {  // credentials only via POST /api/wifi
    body["wifi"].remove("ssid");
    body["wifi"].remove("password");
    body["wifi"].remove("password_set");
  }
  Config c = app::config();
  std::string err;
  if (!cfgjson::mergeApi(body, c, err)) return sendErr(r, 400, err.c_str());
  if (!app::saveConfig(c, err)) return sendErr(r, 400, err.c_str());
  sendConfig(r);
}

// ---------------------------------------------------------------- /api/logic
void sendLogic(AsyncWebServerRequest* r) {
  JsonDocument d;
  std::vector<std::string> warn;
  app::withLogic([&](const logic::LogicConfig& l) {
    logic::toJson(l, d["logic"].to<JsonObject>());
    warn = logic::overlapWarnings(l);
  });
  JsonArray w = d["warnings"].to<JsonArray>();
  for (auto& m : warn) w.add(m);
  RuntimeState rt = app::runtime();
  Config c = app::config();
  float minp = NAN;
  app::withPrices([&](const PriceSnapshot& p) { minp = p.min_price; });
  d["sticky_state"] = (const char*)rt.charging_state;
  d["min_offset"] = c.min_offset;
  d["min_price"] = minp;
  sendJson(r, 200, d);
}

void putLogic(AsyncWebServerRequest* r, JsonObject body) {
  logic::LogicConfig lc;
  std::vector<std::string> errs;
  JsonVariantConst src = body;
  if (body["logic"].is<JsonObject>()) src = body["logic"];
  if (!logic::fromJson(src, lc, &errs)) {
    JsonDocument d;
    d["error"] = "invalid logic";
    JsonArray a = d["errors"].to<JsonArray>();
    for (auto& e : errs) a.add(e);
    return sendJson(r, 400, d);
  }
  logic::sortBlocks(lc);
  std::string err;
  if (!app::saveLogic(lc, err)) return sendErr(r, 400, err.c_str());
  sendLogic(r);
}

// ---------------------------------------------------------------- /api/override
void postOverride(AsyncWebServerRequest* r, JsonObject body) {
  Mode m;
  if (!body["mode"].is<const char*>() || !modeFromName(body["mode"].as<const char*>(), m))
    return sendErr(r, 400, "unknown mode");
  Override o;
  o.mode = m;
  if (!body["until"].isNull()) {
    if (!body["until"].is<int64_t>()) return sendErr(r, 400, "until must be an epoch");
    if (m != Mode::ManualOn && m != Mode::ManualOff) return sendErr(r, 400, "until only applies to manual modes");
    if (!app::clockValid()) return sendErr(r, 503, "clock not set yet");
    int64_t u = body["until"].as<int64_t>();
    if (u <= sysclock::now()) return sendErr(r, 400, "until is in the past");
    o.until = u;
  }
  std::string err;
  if (!app::setOverride(o, err)) return sendErr(r, 400, err.c_str());
  Override cur = app::getOverride();
  JsonDocument d;
  d["mode"] = modeName(cur.mode);
  setEpoch(d["until"], cur.until);
  d["relay"] = app::relayOn();
  d["reason"] = (const char*)app::decision().reason;
  sendJson(r, 200, d);
}

// ---------------------------------------------------------------- stats & log
void handleStats(AsyncWebServerRequest* r, bool detail) {
  if (!app::clockValid()) return sendRaw(r, 200, "[]");
  uint32_t now = (uint32_t)sysclock::now();
  uint32_t from = now - 86400, to = now;
  queryU32(r, "from", from);
  queryU32(r, "to", to);
  sendChunked(r, [=](uint32_t& cur, char* b, size_t cap) {
    return detail ? stats::fillDetail(from, to, cur, b, cap) : stats::fillHourly(from, to, cur, b, cap);
  });
}

void handleSessions(AsyncWebServerRequest* r) {
  uint32_t limit = 20;
  queryU32(r, "limit", limit);
  if (limit < 1) limit = 1;
  if (limit > 200) limit = 200;
  sendChunked(r, [limit](uint32_t& cur, char* b, size_t cap) { return stats::fillSessions((uint16_t)limit, cur, b, cap); });
}

void handleAgg(AsyncWebServerRequest* r, bool monthly) {
  uint32_t n = monthly ? 6 : 14;
  queryU32(r, monthly ? "months" : "days", n);
  uint32_t maxN = monthly ? 36 : 62;
  if (n < 1) n = 1;
  if (n > maxN) n = maxN;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  if (app::clockValid()) {
    stats::Agg rows[62];
    size_t c = monthly ? stats::monthly((uint16_t)n, rows, 62) : stats::daily((uint16_t)n, rows, 62);
    for (size_t i = 0; i < c; i++) {
      JsonObject o = a.add<JsonObject>();
      o[monthly ? "month" : "date"] = rows[i].key;
      o["kwh"] = rows[i].kwh;
      o["cost_c"] = rows[i].cost_c;
      o["relay_h"] = rows[i].relay_h10 / 10.0f;
    }
  }
  sendJson(r, 200, d);
}

void handleLog(AsyncWebServerRequest* r) {
  uint32_t limit = 100;
  queryU32(r, "limit", limit);
  if (limit < 1) limit = 1;
  if (limit > 500) limit = 500;
  String kind = r->hasParam("kind") ? r->getParam("kind")->value() : "";
  bool relayGroup = kind == "relay";  // "relay" also includes "state"
  if (kind.length() && !relayGroup) {
    bool ok = false;
    for (int i = 0; i <= (int)evlog::Kind::Err; i++)
      if (kind == evlog::kindName((evlog::Kind)i)) ok = true;
    if (!ok) return sendErr(r, 400, "unknown kind");
  }
  AsyncResponseStream* s = r->beginResponseStream("application/json");
  s->addHeader("Cache-Control", "no-store");
  s->print('[');
  size_t count = 0, bytes = 0;
  char line[400];
  evlog::forEach(relayGroup ? 500 : limit, (kind.length() && !relayGroup) ? kind.c_str() : nullptr,
                 [&](const evlog::Entry& e) {
                   if (relayGroup && e.k != evlog::Kind::Relay && e.k != evlog::Kind::State) return true;
                   JsonDocument d;
                   setEpoch(d["t"], e.t);
                   d["up"] = e.up;
                   d["k"] = evlog::kindName(e.k);
                   d["msg"] = e.msg;
                   size_t n = serializeJson(d, line, sizeof(line));
                   if (n == 0 || bytes + n > kLogMaxBytes) return false;
                   if (count) s->print(',');
                   s->write((const uint8_t*)line, n);
                   bytes += n + 1;
                   return ++count < limit;
                 });
  s->print(']');
  r->send(s);
}

// ---------------------------------------------------------------- OTA / wifi / actions
void handleOtaProgress(AsyncWebServerRequest* r) {
  ota::Progress p = ota::progress();
  JsonDocument d;
  d["phase"] = ota::phaseName(p.phase);
  // queued/running: `phase` may still be the previous result until the loop task starts the job
  d["job"] = app::jobStateName(app::jobs().ota.state);
  d["current"] = FW_VERSION;
  if (p.has_manifest) {
    JsonObject m = d["manifest"].to<JsonObject>();
    m["version"] = (const char*)p.manifest.version;
    m["url"] = (const char*)p.manifest.url;
    m["size"] = p.manifest.size;
    m["sha256"] = (const char*)p.manifest.sha256;
    m["notes"] = (const char*)p.manifest.notes;
    m["direct_bin"] = p.manifest.direct_bin;
  } else {
    d["manifest"] = nullptr;
  }
  d["bytes"] = p.bytes;
  d["total"] = p.total;
  d["pct"] = p.total ? (int)((uint64_t)p.bytes * 100 / p.total) : 0;
  d["error"] = (const char*)p.error;
  sendJson(r, 200, d);
}

void handleWifiScan(AsyncWebServerRequest* r) {
  bool refresh = r->hasParam("refresh") && r->getParam("refresh")->value() == "1";
  if (refresh) {
    wifi::startScan();  // false = already running, fine
    return sendRaw(r, 202, "{\"scanning\":true}");
  }
  if (wifi::scanRunning()) return sendRaw(r, 202, "{\"scanning\":true}");
  wifi::Network nets[24];
  int n = wifi::scanResults(nets, 24);
  if (n < 0) {
    wifi::startScan();
    return sendRaw(r, 202, "{\"scanning\":true}");
  }
  JsonDocument d;
  JsonArray a = d["networks"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = nets[i].ssid;
    o["rssi"] = nets[i].rssi;
    o["secure"] = nets[i].secure;
    o["ch"] = nets[i].channel;
  }
  sendJson(r, 200, d);
}

void postWifi(AsyncWebServerRequest* r, JsonObject body) {
  const char* ssid = body["ssid"].is<const char*>() ? body["ssid"].as<const char*>() : "";
  const char* pass = body["password"].is<const char*>() ? body["password"].as<const char*>() : "";
  size_t sl = strlen(ssid), pl = strlen(pass);
  if (sl == 0 || sl > 32) return sendErr(r, 400, "ssid must be 1-32 characters");
  if (pl != 0 && (pl < 8 || pl > 63)) return sendErr(r, 400, "password must be 8-63 characters");
  if (jobBusy(app::jobs().wifi)) return sendErr(r, 409, "wifi change already in progress");
  app::requestWifi(ssid, pass);
  sendQueued(r, app::jobs().wifi.seq);
}

// Reads an optional "url" (must be https) from the body; returns false after sending an error.
bool bodyUrl(AsyncWebServerRequest* r, JsonObject body, const char*& url) {
  url = "";
  if (body["url"].isNull()) return true;
  if (!body["url"].is<const char*>()) { sendErr(r, 400, "url must be a string"); return false; }
  url = body["url"].as<const char*>();
  if (*url && strncmp(url, "https://", 8) != 0) { sendErr(r, 400, "url must start with https://"); return false; }
  return true;
}

}  // namespace

void registerRoutes(AsyncWebServer& s) {
  s.on("/api/status", HTTP_GET, handleStatus);
  s.on("/api/prices", HTTP_GET, handlePrices);

  s.on("/api/config", HTTP_GET, sendConfig);
  onJson(s, "/api/config", HTTP_PUT, kConfigBody, putConfig);
  s.on("/api/logic", HTTP_GET, sendLogic);
  onJson(s, "/api/logic", HTTP_PUT, kLogicBody, putLogic);
  onJson(s, "/api/override", HTTP_POST, kSmallBody, postOverride);

  s.on("/api/stats/hourly", HTTP_GET, [](AsyncWebServerRequest* r) { handleStats(r, false); });
  s.on("/api/stats/detail", HTTP_GET, [](AsyncWebServerRequest* r) { handleStats(r, true); });
  s.on("/api/stats/sessions", HTTP_GET, handleSessions);
  s.on("/api/stats/daily", HTTP_GET, [](AsyncWebServerRequest* r) { handleAgg(r, false); });
  s.on("/api/stats/monthly", HTTP_GET, [](AsyncWebServerRequest* r) { handleAgg(r, true); });
  s.on("/api/log", HTTP_GET, handleLog);

  onJson(s, "/api/pushover/test", HTTP_POST, kSmallBody, [](AsyncWebServerRequest* r, JsonObject) {
    if (jobBusy(app::jobs().push_test)) return sendErr(r, 409, "test already running");
    std::string err;
    if (!app::requestPushoverTest(err)) return sendErr(r, 400, err.empty() ? "pushover not configured" : err.c_str());
    sendQueued(r, app::jobs().push_test.seq);
  });
  onJson(s, "/api/car/poll", HTTP_POST, kSmallBody, [](AsyncWebServerRequest* r, JsonObject) {
    if (jobBusy(app::jobs().car_poll)) return sendErr(r, 409, "poll already running");
    app::requestCarPoll();
    sendQueued(r, app::jobs().car_poll.seq);
  });
  onJson(s, "/api/ota/check", HTTP_POST, kSmallBody, [](AsyncWebServerRequest* r, JsonObject b) {
    const char* url;
    if (!bodyUrl(r, b, url)) return;
    if (jobBusy(app::jobs().ota)) return sendErr(r, 409, "OTA already running");
    std::string err;
    if (!app::requestOtaCheck(url, err)) return sendErr(r, 400, err.empty() ? "cannot check" : err.c_str());
    sendQueued(r, app::jobs().ota.seq);
  });
  onJson(s, "/api/ota/install", HTTP_POST, kSmallBody, [](AsyncWebServerRequest* r, JsonObject b) {
    const char* url;
    if (!bodyUrl(r, b, url)) return;
    if (jobBusy(app::jobs().ota)) return sendErr(r, 409, "OTA already running");
    if (!*url && !ota::progress().has_manifest) return sendErr(r, 409, "check for an update first");
    std::string err;
    if (!app::requestOtaInstall(url, err)) return sendErr(r, 400, err.empty() ? "cannot install" : err.c_str());
    sendQueued(r, app::jobs().ota.seq);
  });
  s.on("/api/ota/progress", HTTP_GET, handleOtaProgress);
  onJson(s, "/api/reboot", HTTP_POST, kSmallBody, [](AsyncWebServerRequest* r, JsonObject) {
    app::requestReboot();
    sendRaw(r, 202, "{\"rebooting\":true}");
  });
  s.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  onJson(s, "/api/wifi", HTTP_POST, kSmallBody, postWifi);
}

}  // namespace api
