// STUB — owner: integrator. Wires the minimum to boot: relay default, LittleFS, Wi-Fi, web.
#include "app/app.h"

#include <Arduino.h>
#include <LittleFS.h>

#include "io/relay.h"
#include "net/clock.h"
#include "net/net_lock.h"
#include "net/wifi_mgr.h"
#include "store/config_store.h"
#include "store/event_log.h"
#include "web/server.h"

namespace app {

static Config s_cfg;
static RuntimeState s_rt;
static Decision s_dec;
static CarSnapshot s_car;
static logic::LogicConfig s_logic;

void setup() {
  relay::begin(true, false);  // before anything else: factory default OFF, active-high
  Serial.begin(115200);
  netlock::init();
  LittleFS.begin(true, "/littlefs", 10, "littlefs");
  evlog::begin();
  cfgstore::begin();
  cfgstore::loadConfig(s_cfg);
  relay::setPolarity(s_cfg.relay_active_high);
  relay::set(s_cfg.default_relay);
  evlog::add(evlog::Kind::Sys, "Boot %s", FW_VERSION);
  wifi::begin(s_cfg.wifi_timeout_s);
  sysclock::begin();
  web::begin();
}

void loop() {
  wifi::loop();
  web::loop();
  delay(10);
}

Config config() { return s_cfg; }
Override getOverride() { return s_rt.ovr; }
Decision decision() { return s_dec; }
CarSnapshot car() { return s_car; }
RuntimeState runtime() { return s_rt; }
bool relayOn() { return relay::get(); }
bool clockValid() { return sysclock::valid(); }
void withPrices(const std::function<void(const PriceSnapshot&)>& fn) { static PriceSnapshot p; fn(p); }
void withLogic(const std::function<void(const logic::LogicConfig&)>& fn) { fn(s_logic); }
const char* resetReason() { return "unknown"; }
const char* jobStateName(JobState s) {
  static const char* const n[] = {"idle", "queued", "running", "ok", "error"};
  return n[(int)s];
}
Jobs jobs() { return Jobs(); }
bool setOverride(const Override&, std::string& err) { err = "not implemented"; return false; }
bool saveConfig(const Config&, std::string& err) { err = "not implemented"; return false; }
bool saveLogic(const logic::LogicConfig&, std::string& err) { err = "not implemented"; return false; }
void requestCarPoll() {}
bool requestPushoverTest(std::string& err) { err = "not implemented"; return false; }
bool requestOtaCheck(const char*, std::string& err) { err = "not implemented"; return false; }
bool requestOtaInstall(const char*, std::string& err) { err = "not implemented"; return false; }
void requestReboot() {}
void requestWifi(const char*, const char*) {}

}  // namespace app
