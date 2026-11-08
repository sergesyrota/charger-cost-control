// App orchestrator (see app.h). One cooperative scheduler on the Arduino loop task; every blocking
// job (TLS, BLE, OTA, Wi-Fi change) runs here, one at a time. Web handlers (AsyncTCP task) only read
// copies under s_mx or queue work; override changes are evaluated synchronously (pure + GPIO + NVS).
// Owner: integrator.
#include "app/app.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "car/car_link.h"
#include "io/relay.h"
#include "logic/decision_engine.h"
#include "net/clock.h"
#include "net/net_lock.h"
#include "net/wifi_mgr.h"
#include "notify/pushover.h"
#include "ota/updater.h"
#include "price/price_fetcher.h"
#include "price/price_math.h"
#include "store/config_store.h"
#include "store/event_log.h"
#include "store/state_store.h"
#include "store/stats.h"
#include "web/server.h"

namespace app {
namespace {

SemaphoreHandle_t s_mx = nullptr;
struct Lock {
  Lock() { xSemaphoreTakeRecursive(s_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(s_mx); }
};

// ---- state (all under s_mx)
Config s_cfg;
RuntimeState s_rt;
Decision s_dec;
CarSnapshot s_car;
logic::LogicConfig s_logic;
PriceSnapshot s_prices;  // app copy of prices::snapshot() + derived fields (web reads this one)
Jobs s_jobs;
int64_t s_full_since = 0;

// boot: the first real (non-default) decision continues from the pre-reboot relay (hysteresis) and
// a decision that only restores it is not notified. Limited to kBootQuietMs and non-manual changes.
constexpr uint32_t kBootQuietMs = 15u * 60u * 1000u;
bool s_boot_restore = true;
bool s_boot_relay = false;

// pending ON notification (sent after the post-ON car probe)
bool s_on_pending = false;
bool s_on_manual = false;
Decision s_on_dec;

// queued work
bool s_apply_cfg = false;
bool s_stats_due = false;
bool s_poll_req = false;
bool s_push_req = false;
bool s_ota_check_req = false, s_ota_install_req = false;
char s_ota_url[192] = "";
bool s_wifi_req = false;
char s_wifi_ssid[33] = "", s_wifi_pass[65] = "";
uint32_t s_reboot_at = 0;  // millis, 0 = none

// scheduler (millis)
uint32_t s_next_poll = 30000;  // first car poll ~30 s after boot (SOC for the UI)
bool s_probe = false;          // next poll is the post-ON probe (due at s_next_poll)
uint32_t s_probe_gen = 0;      // bumped on every ON transition (probe armed)
uint32_t s_wifi_wait = 0;      // millis deadline of a pending Wi-Fi join job (0 = none)
char s_wifi_wait_ssid[33] = "";
// Overlays (full / SOC target) trust the last known SOC until the car is seen leaving (poll ->
// not_detected). A full car parked asleep stays blocked (no ON/OFF churn); a car that left and came
// back asleep isn't locked out by a stale 100 %. RAM-only: the first poll after boot re-derives it.
static bool s_soc_departed = false;
uint32_t s_last_min = 0, s_last_heap = 0;
uint32_t s_extras_day = 0;
bool s_clock_seen = false;
bool s_healthy = false, s_health_done = false;
char s_price_err[48] = "";
CarStatus s_last_car_status = CarStatus::Unknown;
char s_reset[24] = "unknown";

bool due(uint32_t at) { return (int32_t)(millis() - at) >= 0; }

int64_t jobTime() {
  int64_t t = sysclock::now();
  return t ? t : sysclock::uptimeS();
}

void jobDone(Job& j, bool ok, const char* err) {
  Lock l;
  j.state = ok ? JobState::Ok : JobState::Error;
  strlcpy(j.error, ok ? "" : (err ? err : "failed"), sizeof(j.error));
  j.finished_at = jobTime();
}

void jobQueued(Job& j) {  // under s_mx
  j.state = JobState::Queued;
  j.error[0] = 0;
  j.seq++;
}

void jobRunning(Job& j) {
  Lock l;
  j.state = JobState::Running;
  j.error[0] = 0;
}

const char* resetName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "Power-on";
    case ESP_RST_EXT: return "External reset";
    case ESP_RST_SW: return "Software reset";
    case ESP_RST_PANIC: return "Crash (panic)";
    case ESP_RST_INT_WDT: return "Interrupt watchdog";
    case ESP_RST_TASK_WDT: return "Task watchdog";
    case ESP_RST_WDT: return "Watchdog";
    case ESP_RST_DEEPSLEEP: return "Deep sleep";
    case ESP_RST_BROWNOUT: return "Brownout";
    case ESP_RST_SDIO: return "SDIO";
    case ESP_RST_USB: return "USB reset";
    case ESP_RST_JTAG: return "JTAG";
    default: return "unknown";
  }
}

void queuePush(const char* msg) {
  if (pushover::ready()) pushover::queue(msg);
}

void sendOnMessage() {  // under s_mx
  if (!s_on_pending) return;
  s_on_pending = false;
  char msg[320];
  pushover::formatOn(s_on_dec, s_on_manual, s_car, msg, sizeof(msg));
  queuePush(msg);
}

// Evaluate the decision engine and apply it. Callable from the loop task and from web handlers
// (override changes). `manual` = triggered by a UI action (prefix "Manual: ").
void evaluate(bool manual) {
  Lock l;
  ChicagoTime ct;
  bool clk = sysclock::chicago(ct);
  int64_t now = clk ? ct.epoch : 0;

  if (logic::overrideExpired(s_rt.ovr, clk, now)) {
    evlog::add(evlog::Kind::Sys, "Timed %s expired → auto", modeName(s_rt.ovr.mode));
    s_rt.ovr = Override();
  }
  float soc = !isnan(s_car.soc) ? s_car.soc : s_rt.last_soc;
  bool soc_fresh = s_car.soc_live || !s_soc_departed;
  if (!isnan(soc) && lroundf(soc) >= 100) {
    if (!s_full_since && clk) s_full_since = now;
  } else {
    s_full_since = 0;
  }

  logic::DecisionInput in;
  in.cfg = &s_cfg;
  in.logic = &s_logic;
  in.ovr = s_rt.ovr;
  in.clock_valid = clk;
  if (clk) in.now = ct;
  in.price = s_prices.current;
  in.min_price = s_prices.min_price;
  in.soc = soc;
  in.soc_fresh = soc_fresh;
  const bool prev = relay::get();
  bool boot = s_boot_restore;
  if (boot && (manual || millis() > kBootQuietMs)) boot = s_boot_restore = false;
  const bool was = boot ? s_boot_relay : prev;  // logical relay before this decision (hysteresis)
  in.relay_now = was;
  in.sticky_state = s_rt.charging_state;
  in.full_since = s_full_since;
  Decision d = logic::decide(in);
  s_dec = d;

  if (d.src == DecisionSrc::Price && d.state[0] && strcmp(d.state, s_rt.charging_state) != 0) {
    evlog::add(evlog::Kind::State, "State %s → %s (%s)", s_rt.charging_state, d.state, stateHowName(d.how));
    strlcpy(s_rt.charging_state, d.state, sizeof(s_rt.charging_state));
  }

  const bool is_manual = manual || d.src == DecisionSrc::Manual;
  const bool pin_change = d.relay != prev;
  if (pin_change) {
    relay::set(d.relay);
    s_rt.relay_cmd = d.relay;
    s_stats_due = true;
  }
  if (boot && d.src == DecisionSrc::Default) {
    // still nothing known after boot: wait for a real decision before judging transitions
    if (pin_change) evlog::add(evlog::Kind::Relay, "%s: %s (start-up, not notified)", d.relay ? "ON" : "OFF", d.reason);
  } else if (d.relay != was) {  // a real transition (vs. the pre-reboot relay while booting)
    s_boot_restore = false;
    evlog::add(evlog::Kind::Relay, "%s%s: %s%s", is_manual ? "Manual: " : "", d.relay ? "ON" : "OFF", d.reason,
               pin_change ? "" : " (after reboot)");
    if (d.relay) {
      // ON message waits for the post-ON probe (§8.5/§10)
      s_on_pending = true;
      s_on_dec = d;
      s_on_manual = is_manual;
      s_probe = true;
      s_probe_gen++;
      s_next_poll = millis() + (uint32_t)s_cfg.probe_delay_s * 1000u;
    } else {
      if (s_on_pending) sendOnMessage();  // flapped before the probe: still report the ON
      char msg[320];
      pushover::formatOff(d, is_manual, s_car, now, msg, sizeof(msg));
      queuePush(msg);
      s_probe = false;
      s_next_poll = millis() + (uint32_t)s_cfg.poll_off_min * 60000u;
    }
  } else {
    if (boot) s_boot_restore = false;  // first real decision agrees with the pre-reboot relay
    if (pin_change) {                  // ... and restores it: log, don't notify
      evlog::add(evlog::Kind::Relay, "%s: %s (restored after reboot, not notified)", d.relay ? "ON" : "OFF", d.reason);
      s_probe = false;
      s_next_poll = millis() + (d.relay ? (uint32_t)s_cfg.probe_delay_s * 1000u : (uint32_t)s_cfg.poll_off_min * 60000u);
    }
  }
  statestore::save(s_rt);
}

void applyConfig() {  // loop task
  Config c;
  {
    Lock l;
    c = s_cfg;
  }
  relay::setPolarity(c.relay_active_high);
  wifi::setTimeout(c.wifi_timeout_s);
  pushover::configure({c.pushover_enabled, c.pushover_token, c.pushover_user, c.pushover_device});
  carlink::setMac(c.dongle_mac);
  memset(c.pushover_token, 0, sizeof(c.pushover_token));
  memset(c.pushover_user, 0, sizeof(c.pushover_user));
}

void refreshPrices(const ChicagoTime& ct, bool copy_raw) {
  Lock l;
  if (copy_raw) s_prices = prices::snapshot();
  price::derive(s_prices, ct, s_cfg.stale_min, s_cfg.min_offset);
}

void logPriceError() {  // loop task (the only writer of prices::lastError())
  char e[sizeof(s_price_err)];
  strlcpy(e, prices::lastError(), sizeof(e));
  {
    Lock l;
    if (strcmp(e, s_price_err) == 0) return;
    strlcpy(s_price_err, e, sizeof(s_price_err));
  }
  if (*e) evlog::add(evlog::Kind::Price, "Price fetch failed: %s", e);
  else evlog::add(evlog::Kind::Price, "Price fetch OK");
}

void doCarPoll(bool user) {
  carlink::PollOpts o;
  CarSnapshot c;
  bool probe;
  uint32_t gen;
  {
    Lock l;
    c = s_car;
    probe = s_probe && due(s_next_poll);  // a user poll before the probe delay is not the probe
    gen = s_probe_gen;
    o.threshold_w = s_cfg.charge_threshold_w;
    o.power_sign = s_cfg.power_sign;
  }
  ChicagoTime ct;
  bool clk = sysclock::chicago(ct);
  o.extras = clk && ct.yyyymmdd() != s_extras_day;
  if (user) jobRunning(s_jobs.car_poll);
  bool answered = carlink::poll(c, o, clk ? ct.epoch : 0);
  carlink::end();  // release the BLE host + controller (~60 KB) between polls so TLS has room
  if (answered && o.extras && (c.status == CarStatus::Online || c.status == CarStatus::Charging))
    s_extras_day = ct.yyyymmdd();
  {
    Lock l;
    s_car = c;
    if (c.soc_live && !isnan(c.soc)) {
      s_rt.last_soc = c.soc;
      s_rt.last_soc_at = c.soc_at;
      s_soc_departed = false;
    } else if (c.status == CarStatus::NotDetected) {
      s_soc_departed = true;
    }
    // A probe this poll didn't serve (armed by an ON during the poll, or not yet due) keeps the
    // s_probe / s_next_poll that evaluate() set.
    if (!(s_probe && !(probe && s_probe_gen == gen))) {
      bool on = relay::get();
      s_next_poll = millis() + (on ? (uint32_t)s_cfg.poll_on_s * 1000u : (uint32_t)s_cfg.poll_off_min * 60000u);
      s_probe = false;
      if (probe) sendOnMessage();
    }
  }
  if (c.status != s_last_car_status) {
    if (isnan(c.soc))
      evlog::add(evlog::Kind::Car, "Car %s%s%s", carStatusName(c.status), c.error[0] ? ": " : "", c.error);
    else
      evlog::add(evlog::Kind::Car, "Car %s, SOC %.0f%%%s%s", carStatusName(c.status), c.soc, c.error[0] ? ", " : "",
                 c.error);
    s_last_car_status = c.status;
  }
  if (user) jobDone(s_jobs.car_poll, answered, c.error[0] ? c.error : "car did not answer");
  evaluate(false);  // §7.1 (c)
  s_stats_due = true;
}

void sampleStats() {
  ChicagoTime ct;
  bool clk = sysclock::chicago(ct);
  PriceSnapshot* p = &s_prices;  // only the loop task writes s_prices
  CarSnapshot c;
  Mode m;
  {
    Lock l;
    c = s_car;
    m = s_rt.ovr.mode;
  }
  stats::sample(clk ? &ct : nullptr, *p, c, relay::get(), m);
}

void flushAll() {
  stats::flush();
  Lock l;  // statestore is not thread-safe: web handlers save() under the same lock
  statestore::save(s_rt);
  statestore::flush();
}

void runJobs() {
  bool push, chk, inst, wf;
  char url[192];
  {
    Lock l;
    push = s_push_req;
    chk = s_ota_check_req;
    inst = s_ota_install_req;
    wf = s_wifi_req;
    s_push_req = s_ota_check_req = s_ota_install_req = s_wifi_req = false;
    strlcpy(url, s_ota_url, sizeof(url));
  }
  if (push) {
    jobRunning(s_jobs.push_test);
    char err[80] = "";
    bool ok = pushover::sendNow("Test from ChargerControl " FW_VERSION, err, sizeof(err));
    evlog::add(ok ? evlog::Kind::Push : evlog::Kind::Err, "Pushover test %s%s%s", ok ? "sent" : "failed", ok ? "" : ": ", err);
    jobDone(s_jobs.push_test, ok, err);
  }
  if (chk) {
    jobRunning(s_jobs.ota);
    bool ok = ota::check(url);
    ota::Progress p = ota::progress();
    jobDone(s_jobs.ota, ok, p.error);
  }
  if (inst) {
    jobRunning(s_jobs.ota);
    evlog::add(evlog::Kind::Sys, "OTA install started");
    bool ok = ota::install(url);
    ota::Progress p = ota::progress();
    jobDone(s_jobs.ota, ok, p.error);
    if (ok) {
      evlog::add(evlog::Kind::Sys, "OTA installed %s, rebooting", p.manifest.version);
      flushAll();
      delay(500);
      ESP.restart();
    } else {
      evlog::add(evlog::Kind::Err, "OTA failed: %s", p.error);
    }
  }
  if (wf) {
    jobRunning(s_jobs.wifi);
    char ssid[33], pass[65];
    {
      Lock l;
      strlcpy(ssid, s_wifi_ssid, sizeof(ssid));
      strlcpy(pass, s_wifi_pass, sizeof(pass));
      memset(s_wifi_pass, 0, sizeof(s_wifi_pass));
    }
    wifi::setCredentials(ssid, pass);
    memset(pass, 0, sizeof(pass));
    evlog::add(evlog::Kind::Sys, ssid[0] ? "Wi-Fi credentials saved for \"%s\"" : "Wi-Fi credentials cleared%s", ssid);
    if (!ssid[0]) {
      jobDone(s_jobs.wifi, true, nullptr);
    } else {  // finished by checkWifiJob() once STA gets an IP (or times out)
      strlcpy(s_wifi_wait_ssid, ssid, sizeof(s_wifi_wait_ssid));
      s_wifi_wait = millis() + 45000;
      if (!s_wifi_wait) s_wifi_wait = 1;
    }
  }
}

void checkWifiJob() {
  if (!s_wifi_wait) return;
  if (wifi::staConnected() && wifi::ssid() == s_wifi_wait_ssid) {
    s_wifi_wait = 0;
    jobDone(s_jobs.wifi, true, nullptr);
  } else if (due(s_wifi_wait)) {
    s_wifi_wait = 0;
    jobDone(s_jobs.wifi, false, "could not connect (check the password); still retrying");
  }
}

void healthCheck() {
  if (s_health_done) return;
  bool fetched;
  {
    Lock l;
    fetched = s_prices.five_fetched_at != 0;
  }
  if (wifi::staConnected() && fetched && web::serving()) {
    if (ota::pendingVerify()) evlog::add(evlog::Kind::Sys, "Health check passed; firmware marked valid");
    ota::markValid();
    s_healthy = s_health_done = true;
  } else if (millis() > 5u * 60u * 1000u) {
    s_health_done = true;
    if (ota::pendingVerify()) {
      evlog::add(evlog::Kind::Err, "Health check failed within 5 min; rolling back");
      flushAll();
      ota::rollbackIfPending();  // reboots
    }
  }
}

void logHeap() {
  Serial.printf("[heap] free=%u min=%u largest=%u up=%us wifi=%s ap=%s relay=%s\n", (unsigned)ESP.getFreeHeap(),
                (unsigned)ESP.getMinFreeHeap(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                (unsigned)sysclock::uptimeS(), wifi::staConnected() ? wifi::ip().c_str() : "-",
                wifi::apActive() ? wifi::apSsid().c_str() : "-", relay::get() ? "ON" : "OFF");
}

}  // namespace

// ================================================================ setup / loop
void setup() {
  {
    Config boot;  // FIRST: drive the configured default level/polarity (NVS read only, a few ms)
    cfgstore::peekConfig(boot);
    relay::begin(boot.relay_active_high, boot.default_relay);
  }
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block a task on the USB console (no host reading => drop)
  s_mx = xSemaphoreCreateRecursiveMutex();
  strlcpy(s_reset, resetName(esp_reset_reason()), sizeof(s_reset));
  netlock::init();
  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) Serial.println("LittleFS mount failed");
  evlog::begin();
  cfgstore::begin();
  cfgstore::loadConfig(s_cfg);
  cfgstore::loadLogic(s_logic);
  statestore::load(s_rt);
  relay::setPolarity(s_cfg.relay_active_high);
  relay::set(s_cfg.default_relay);  // §13: default until clock + prices are known
  if (!s_logic.find(s_rt.charging_state)) strlcpy(s_rt.charging_state, "default", sizeof(s_rt.charging_state));
  s_boot_relay = s_rt.relay_cmd;
  s_rt.relay_cmd = relay::get();
  if (!isnan(s_rt.last_soc)) {
    s_car.soc = s_rt.last_soc;
    s_car.soc_at = s_rt.last_soc_at;
  }
  evlog::add(evlog::Kind::Sys, "Boot %s, reset: %s, relay default %s", FW_VERSION, s_reset,
             s_cfg.default_relay ? "ON" : "OFF");
  if (ota::runningFromRollback()) evlog::add(evlog::Kind::Err, "Running from rollback (previous update failed)");

  stats::begin();
  wifi::begin(s_cfg.wifi_timeout_s);
  sysclock::begin();
  web::begin();
  carlink::setMac(s_cfg.dongle_mac);  // BLE is brought up only for the duration of each poll
  prices::begin();
  applyConfig();

  evaluate(false);
  Serial.printf("Decision: %s\n", s_dec.reason);
  logHeap();

  esp_task_wdt_config_t wc = {};
  wc.timeout_ms = 60000;  // > worst-case unfed blocking step (HTTPS connect+handshake+headers, BLE)
  wc.idle_core_mask = 0;
  wc.trigger_panic = true;
  if (esp_task_wdt_reconfigure(&wc) != ESP_OK) esp_task_wdt_init(&wc);
  enableLoopWDT();  // Arduino feeds it after every loop() iteration
}

void loop() {
  wifi::loop();
  web::loop();

  bool apply, statsdue, pollreq;
  {
    Lock l;
    apply = s_apply_cfg;
    s_apply_cfg = false;
    pollreq = s_poll_req;
    s_poll_req = false;
  }
  if (apply) {
    applyConfig();
    ChicagoTime ct;
    if (sysclock::chicago(ct)) refreshPrices(ct, false);  // stale_min / min_offset may have changed
    evaluate(false);
  }

  // clock + prices
  ChicagoTime ct;
  if (sysclock::chicago(ct)) {
    if (!s_clock_seen) {
      s_clock_seen = true;
      evlog::add(evlog::Kind::Sys, "Clock set (NTP)");
      prices::forceFiveMin();
    }
    bool changed = prices::tick(ct);  // at most one blocking fetch
    logPriceError();
    if (changed) {
      sysclock::chicago(ct);
      refreshPrices(ct, true);
      evaluate(false);  // §7.1 (a)
    }
  }

  // once a minute: time-based re-evaluation (§7.1 d), stats, override expiry
  if (due(s_last_min + 60000)) {
    s_last_min = millis();
    if (sysclock::chicago(ct)) refreshPrices(ct, false);
    evaluate(false);
    s_stats_due = true;
  }

  // car link
  bool pollnow;
  {
    Lock l;
    pollnow = pollreq || due(s_next_poll);
  }
  if (pollnow) doCarPoll(pollreq);

  {
    Lock l;
    statsdue = s_stats_due;
    s_stats_due = false;
  }
  if (statsdue) sampleStats();

  pushover::loop();
  runJobs();
  checkWifiJob();
  healthCheck();

  if (s_reboot_at && due(s_reboot_at)) {
    evlog::add(evlog::Kind::Sys, "Reboot requested");
    flushAll();
    delay(200);
    ESP.restart();
  }
  if (due(s_last_heap + 60000)) {
    s_last_heap = millis();
    logHeap();
  }
  delay(10);
}

// ================================================================ reads
Config config() {
  Lock l;
  return s_cfg;
}
Override getOverride() {
  Lock l;
  return s_rt.ovr;
}
Decision decision() {
  Lock l;
  return s_dec;
}
CarSnapshot car() {
  Lock l;
  return s_car;
}
RuntimeState runtime() {
  Lock l;
  return s_rt;
}
bool relayOn() { return relay::get(); }
bool clockValid() { return sysclock::valid(); }
void withPrices(const std::function<void(const PriceSnapshot&)>& fn) {
  Lock l;
  fn(s_prices);
}
void withLogic(const std::function<void(const logic::LogicConfig&)>& fn) {
  Lock l;
  fn(s_logic);
}
const char* resetReason() { return s_reset; }
void priceError(char* out, size_t n) {
  Lock l;
  strlcpy(out, s_price_err, n);
}
const char* jobStateName(JobState s) {
  static const char* const n[] = {"idle", "queued", "running", "ok", "error"};
  return n[(int)s];
}
Jobs jobs() {
  Lock l;
  return s_jobs;
}

// ================================================================ actions
bool setOverride(const Override& o, std::string& err) {
  bool manual_mode = o.mode == Mode::ManualOn || o.mode == Mode::ManualOff;
  if (o.until && !manual_mode) return err = "until only applies to manual modes", false;
  if (o.until) {
    int64_t now = sysclock::now();
    if (!now) return err = "clock not set yet", false;
    if (o.until <= now) return err = "until is in the past", false;
  }
  Lock l;
  s_rt.ovr = o;
  evlog::add(evlog::Kind::Sys, "Mode → %s%s", modeName(o.mode), o.until ? " (timed)" : "");
  evaluate(true);
  return true;
}

bool saveConfig(const Config& c, std::string& err) {
  if (!cfgstore::saveConfig(c)) return err = "could not save config", false;
  Lock l;
  s_cfg = c;
  s_apply_cfg = true;
  evlog::add(evlog::Kind::Sys, "Settings saved");
  return true;
}

bool saveLogic(const logic::LogicConfig& lc, std::string& err) {
  logic::LogicConfig copy = lc;
  logic::sortBlocks(copy);
  if (!cfgstore::saveLogic(copy)) return err = "could not save rules", false;
  Lock l;
  s_logic = std::move(copy);
  if (!s_logic.find(s_rt.charging_state)) strlcpy(s_rt.charging_state, "default", sizeof(s_rt.charging_state));
  evlog::add(evlog::Kind::Sys, "Charging rules saved");
  evaluate(false);
  return true;
}

void requestCarPoll() {
  Lock l;
  s_poll_req = true;
  jobQueued(s_jobs.car_poll);
}

bool requestPushoverTest(std::string& err) {
  Lock l;
  if (!s_cfg.pushover_token[0] || !s_cfg.pushover_user[0]) return err = "pushover token/user not set", false;
  s_push_req = true;
  jobQueued(s_jobs.push_test);
  return true;
}

bool requestOtaCheck(const char* url, std::string& err) {
  Lock l;
  const char* u = (url && *url) ? url : s_cfg.ota_url;
  if (strncmp(u, "https://", 8) != 0) return err = "OTA URL must be https://", false;
  if (strlen(u) >= sizeof(s_ota_url)) return err = "URL too long", false;
  strlcpy(s_ota_url, u, sizeof(s_ota_url));
  s_ota_check_req = true;
  jobQueued(s_jobs.ota);
  return true;
}

bool requestOtaInstall(const char* url, std::string& err) {
  Lock l;
  const char* u = url ? url : "";
  if (*u && strncmp(u, "https://", 8) != 0) return err = "OTA URL must be https://", false;
  if (strlen(u) >= sizeof(s_ota_url)) return err = "URL too long", false;
  if (!*u && !ota::progress().has_manifest) return err = "check for an update first", false;
  strlcpy(s_ota_url, u, sizeof(s_ota_url));
  s_ota_install_req = true;
  jobQueued(s_jobs.ota);
  return true;
}

void requestReboot() {
  Lock l;
  s_reboot_at = millis() + 1000;
  if (!s_reboot_at) s_reboot_at = 1;
}

void requestWifi(const char* ssid, const char* password) {
  Lock l;
  strlcpy(s_wifi_ssid, ssid ? ssid : "", sizeof(s_wifi_ssid));
  strlcpy(s_wifi_pass, password ? password : "", sizeof(s_wifi_pass));
  s_wifi_req = true;
  s_wifi_wait = 0;
  jobQueued(s_jobs.wifi);
}

}  // namespace app
