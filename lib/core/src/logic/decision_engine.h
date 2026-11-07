// Decision engine — precedence (PLAN §7.1), overlays (§7.2), price logic = port of
// Vehicle::getApplicableLogic + controlCharging (§7.3) with LITERAL operators. Pure. Owner: logic.
//
// Precedence (first applicable wins):
//  1. Manual ON/OFF (Override.mode). Overlays do NOT apply to Manual ON.
//  2. Overlays (auto / schedule / schedule_price): Full (round(soc) >= 100, held full_hold_min) -> OFF
//     "Bolt is full"; SOC target (enabled && soc >= target) -> OFF "SOC target 80% reached".
//     Overlays only when SOC is known (not NAN) and fresh (soc_fresh).
//  3. Schedule (schedule / schedule_price): needs clock; inside a window -> ON. Outside:
//     schedule -> OFF; schedule_price -> fall through to price. A window with max_price forces ON
//     only while the price is known and <= max_price; otherwise it counts as "outside".
//  4. Price logic: needs clock + known price; else Default.
//  5. Default: cfg.default_relay with reason "<why> → relay held at default (OFF)".
// Reason strings follow the prototype decide() and PLAN §10 ("Price is low (1.2¢ ≤ 2¢)",
// "Price is high (3.4¢ > 3¢)", "Not enough confidence yet (:12 < :20)", ...).
#pragma once
#include <stdint.h>

#include "core_types.h"
#include "logic/logic_config.h"

namespace logic {

struct DecisionInput {
  const Config* cfg = nullptr;
  const LogicConfig* logic = nullptr;
  Override ovr;
  bool clock_valid = false;
  ChicagoTime now;               // meaningful only if clock_valid
  float price = NAN;             // PriceSnapshot.current (staleness already applied); NAN = unknown
  float min_price = NAN;         // PriceSnapshot.min_price; NAN = "min" unavailable
  float soc = NAN;               // last known SOC; NAN = never known (logic uses 0, overlays skip)
  bool soc_fresh = true;         // false => SOC is old (car not read recently): overlays skip it
  bool relay_now = false;        // currently commanded relay
  const char* sticky_state = "default";
  int64_t full_since = 0;        // epoch SOC first reached 100 (0 = not full); for full_hold_min
};

// Pure evaluation. Decision.state holds the state to persist as the new sticky state (changes when a
// state was "entered" or forced, or when a nonexistent sticky name was resolved; else = sticky_state). Decision.at = now.epoch or 0.
Decision decide(const DecisionInput& in);

// State selection only (state_select -> forced; else first state whose non-empty conditions all hold
// -> entered; else sticky; a sticky name that doesn't exist, with no states.default, resolves to the
// first state that has blocks). Exposed for the UI/tests.
struct StatePick {
  const State* state = nullptr;      // nullptr if the chosen name doesn't exist
  std::string name;
  StateHow how = StateHow::None;
  const std::vector<Block>* blocks = nullptr;  // own, or states.default's when missing; nullptr if none
  bool fallback = false;
  int block_index = -1;              // first block with soc >= block.soc, else last; -1 if no blocks
};
StatePick pickState(const LogicConfig& cfg, float soc, const char* sticky_state);

// Window containing `now`, or nullptr. Overnight windows (start > end) belong to the start day:
// active if (day in days && m >= start) || (yesterday in days && m < end).
const ScheduleWindow* activeWindow(const Config& cfg, const ChicagoTime& now);

// True if a timed manual override has expired (clock valid && until != 0 && now >= until).
bool overrideExpired(const Override& o, bool clock_valid, int64_t now);

}  // namespace logic
