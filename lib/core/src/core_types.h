// Central data types shared by every module. Pure C++ (no Arduino/ESP headers).
// Owner: integrator. Builders: treat as read-only; ask the integrator for changes.
//
// Conventions
//  - Unknown float = NAN (serialized as JSON null). Unknown epoch = 0.
//  - Prices are supply-only cents/kWh. Epochs are UTC seconds.
//  - Strings are UTF-8 (state names may be Cyrillic: "Заряд"). Fixed char buffers are always NUL-terminated.
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "util/chicago_time.h"

#ifndef FW_VERSION
#define FW_VERSION "0.0.0-dev"
#endif

constexpr const char* kDefaultOtaUrl =
    "https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json";
constexpr size_t kStateNameMax = 48;  // bytes incl. NUL: max 47 bytes (23 Cyrillic chars)
constexpr size_t kMaxWindows = 8;
constexpr size_t kFiveMax = 300;      // 5-min feed holds ~24 h = 288 points

// ---------------------------------------------------------------- modes
enum class Mode : uint8_t { Auto, Schedule, SchedulePrice, ManualOn, ManualOff };
const char* modeName(Mode m);                     // "auto" "schedule" "schedule_price" "manual_on" "manual_off"
bool modeFromName(const char* s, Mode& out);      // false if unknown

// ---------------------------------------------------------------- car
enum class CarStatus : uint8_t { Unknown, NotDetected, Asleep, Online, Charging };
const char* carStatusName(CarStatus s);           // "unknown" "not_detected" "asleep" "online" "charging"

struct CarSnapshot {
  CarStatus status = CarStatus::Unknown;  // Unknown = not polled since boot
  float soc = NAN;            // %, 0..100; last known (may come from NVS when car unreachable)
  bool soc_live = false;      // true if `soc` came from the most recent poll
  int64_t soc_at = 0;         // epoch the SOC was read (0 = unknown / read before clock valid)
  float volts = NAN, amps = NAN, watts = NAN;   // last poll while online; watts = V*I (raw sign)
  int64_t last_poll_at = 0;   // epoch of last poll attempt (0 = none / no clock)
  uint32_t last_poll_up_s = 0;// uptime seconds of last poll attempt (works without clock)
  int64_t last_seen = 0;      // epoch the car last answered (online/asleep/charging)
  float capacity_kwh = NAN, odometer_mi = NAN;  // daily extras
  int32_t charge_count = -1;
  char error[96] = "";        // last transport/parse error (Russian UTF-8), "" if none
};

// ---------------------------------------------------------------- prices
struct FivePoint {
  uint32_t epoch;  // millisUTC / 1000
  float price;     // ¢/kWh
};

struct PriceSnapshot {
  // raw feeds
  FivePoint five[kFiveMax];        // newest first (feed order)
  uint16_t five_count = 0;
  int64_t five_fetched_at = 0;     // last successful 5-min fetch (epoch)
  float da_today[24];              // day-ahead, index = hour-ending (ServletFeed semantics), NAN missing
  uint32_t da_today_date = 0;      // YYYYMMDD Chicago (0 = not loaded)
  float da_tomorrow[24];
  uint32_t da_tomorrow_date = 0;
  int64_t da_fetched_at = 0;
  float actual_today[24];          // display/stats only
  uint32_t actual_date = 0;
  int64_t actual_fetched_at = 0;
  // derived by price::derive()
  float current = NAN;             // currentSupply, rounded 0.1; NAN = unknown (=> decision default)
  bool estimated = false;          // true when current came from day-ahead (rule 3)
  float hour_avg = NAN;            // mean of current-hour 5-min points
  uint16_t known_minutes = 0;      // 5 * points in current Chicago hour
  float lowest7 = NAN;             // lowestBy7amSupply
  float min_price = NAN;           // lowest7 + cfg.min_offset (value of "min" in logic)

  PriceSnapshot() {
    for (int i = 0; i < 24; i++) da_today[i] = da_tomorrow[i] = actual_today[i] = NAN;
  }
};

// ---------------------------------------------------------------- config (NVS "cfg")
struct ScheduleWindow {
  uint8_t days = 0;         // bit0 = Sunday .. bit6 = Saturday
  uint16_t start_min = 0;   // minute of day 0..1439
  uint16_t end_min = 0;     // start > end => overnight (ends next day); start == end => never
  float max_price = NAN;    // optional cap, ¢/kWh supply: forces ON only while price <= cap (NAN = no cap)
};

// Everything user-editable. Price-logic JSON is stored separately (logic::LogicConfig).
// Wi-Fi SSID/password are owned by wifi_mgr (not here).
struct Config {
  // relay & safety
  bool relay_active_high = true;   // verified on bench: GPIO4 HIGH = charger ON
  bool default_relay = false;      // relay when nothing is known (factory OFF)
  uint16_t stale_min = 20;         // 5-min feed older than this => not used
  uint16_t full_hold_min = 0;      // keep OFF this long after "full" (0 = immediate only)
  // price
  float min_offset = 0.5f;         // "min" = lowest7 + min_offset
  // overlays
  bool soc_target_enabled = false;
  uint8_t soc_target = 80;         // %
  // schedule
  ScheduleWindow windows[kMaxWindows];
  uint8_t window_count = 0;
  // car link
  char dongle_mac[18] = "8C:DE:52:DE:B4:A0";
  uint16_t poll_on_s = 60;         // poll period while relay ON
  uint16_t poll_off_min = 60;      // poll period while relay OFF
  uint16_t charge_threshold_w = 500;
  int8_t power_sign = 0;           // 0 = use |W|, +1 / -1 = charging direction
  uint16_t probe_delay_s = 30;     // post-ON probe delay
  // notifications
  bool pushover_enabled = false;
  char pushover_device[32] = "";
  char pushover_token[40] = "";    // SECRET: never leaves the device (API shows token_set)
  char pushover_user[40] = "";     // SECRET
  // wifi
  uint16_t wifi_timeout_s = 300;   // STA tries before hotspot
  // ota
  char ota_url[192] = "https://github.com/sergesyrota/charger-cost-control/releases/latest/download/manifest.json";
};

// ---------------------------------------------------------------- runtime state (NVS "state")
// The active mode. `until` (epoch) only for manual modes and only with a valid clock; 0 = indefinite.
// When `until` passes, mode reverts to Auto. Timed overrides are dropped on boot.
struct Override {
  Mode mode = Mode::Auto;
  int64_t until = 0;
};

struct RuntimeState {
  char charging_state[kStateNameMax] = "default";  // sticky price-logic state
  float last_soc = NAN;                            // last known SOC (persisted; not time)
  int64_t last_soc_at = 0;
  bool relay_cmd = false;                          // last commanded relay
  Override ovr;
};

// ---------------------------------------------------------------- decision
enum class DecisionSrc : uint8_t { Default, Manual, Overlay, Schedule, Price };
const char* decisionSrcName(DecisionSrc s);  // "default" "manual" "overlay" "schedule" "price"

enum class StateHow : uint8_t { None, Forced, Entered, Sticky };
const char* stateHowName(StateHow h);        // "" "forced" "entered" "sticky"

struct Decision {
  bool relay = false;
  DecisionSrc src = DecisionSrc::Default;
  char reason[256] = "";              // UI/Pushover text (Russian, UTF-8), e.g. "Цена низкая (1.2¢ ≤ 2¢)"
  // price-logic context (src == Price, else state[0]==0 / block_index == -1)
  char state[kStateNameMax] = "";     // state used; also the new sticky state to persist
  StateHow how = StateHow::None;
  int8_t block_index = -1;            // index into state's charging_logic as stored
  float block_soc = NAN;              // applied block's soc threshold (for "Bolt/Заряд soc≥40")
  bool logic_fallback = false;        // blocks came from states.default
  float price = NAN;                  // price evaluated
  float price_on = NAN, price_off = NAN;  // resolved ("min" substituted)
  uint8_t known_minutes = 0;          // block gate (0 = none)
  int64_t at = 0;                     // epoch evaluated (0 if no clock)
};
