// App orchestrator: owns all runtime state and runs every blocking job (TLS, BLE, OTA, Wi-Fi
// changes) on the Arduino loop task, one at a time. Owner: integrator.
//
// Threading contract
//  - Getters/withX() are thread-safe (internal state mutex) and cheap; web handlers use them.
//  - request*() / set*() / save*() only validate, update state and queue work; they never block on
//    the network. Results show up in jobs() and in GET /api/status.
//  - Only the loop task touches prices::snapshot(), carlink, https, ota::install, wifi::setCredentials.
//
// Loop responsibilities (integrator): clock -> prices::tick -> price::derive -> logic::decide ->
// relay + statestore + evlog + pushover (ON message after probe_delay_s car probe) -> car poll
// cadence (poll_on_s / poll_off_min) -> stats::sample -> queued jobs -> rollback health check
// (Wi-Fi up + 1 ComEd fetch + web serving within 5 min => ota::markValid()).
#pragma once
#include <stdint.h>

#include <functional>
#include <string>

#include "core_types.h"
#include "logic/logic_config.h"

namespace app {

void setup();   // first line drives the relay to the default level
void loop();

// ---------------------------------------------------------------- reads (thread-safe)
Config config();                 // copy (includes secrets — never serialize it to the API directly)
Override getOverride();
Decision decision();             // last evaluation
CarSnapshot car();
RuntimeState runtime();
bool relayOn();
bool clockValid();
void withPrices(const std::function<void(const PriceSnapshot&)>& fn);    // under lock; keep fn short
void withLogic(const std::function<void(const logic::LogicConfig&)>& fn);
void priceError(char* out, size_t n);   // last price-fetch error ("" = OK), copied under lock
const char* resetReason();       // Russian text, e.g. "Включение питания", "Программный сброс"

enum class JobState : uint8_t { Idle, Queued, Running, Ok, Error };
const char* jobStateName(JobState s);   // "idle" "queued" "running" "ok" "error"
struct Job {
  JobState state = JobState::Idle;
  char error[160] = "";                 // Russian UTF-8
  int64_t finished_at = 0;              // epoch (0 if no clock); uptime used otherwise
  uint16_t seq = 0;                     // bumped each time the job is queued (UI freshness check)
};
struct Jobs {
  Job car_poll, push_test, ota, wifi;
};
Jobs jobs();

// ---------------------------------------------------------------- actions
// Mode change. until != 0 only for manual modes and requires a valid clock. Re-evaluates now;
// a resulting relay change is logged and notified with the "Manual: " prefix.
bool setOverride(const Override& o, std::string& err);

// Replace the whole config (web_api builds it with cfgjson::mergeApi on top of config()).
// Persists, applies side effects (relay polarity, wifi timeout, pushover creds, dongle MAC), re-evaluates.
bool saveConfig(const Config& c, std::string& err);

// Validated + block-sorted logic. Persists, re-evaluates. Sticky state kept if it still exists,
// else reset to "default".
bool saveLogic(const logic::LogicConfig& l, std::string& err);

void requestCarPoll();
bool requestPushoverTest(std::string& err);                // err if pushover not configured
bool requestOtaCheck(const char* url, std::string& err);   // "" => config ota_url; https only
bool requestOtaInstall(const char* url, std::string& err); // "" => last checked manifest
void requestReboot();                                      // flushes stats/state, restarts ~1 s later
void requestWifi(const char* ssid, const char* password);  // password nullptr/"" => keep (same ssid)

}  // namespace app
