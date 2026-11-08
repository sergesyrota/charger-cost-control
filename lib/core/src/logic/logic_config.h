// Price-logic config: the bolt-monitor `logic.json` shape (PLAN §7.3), parse/serialize/validate.
// Pure (ArduinoJson is header-only and allowed). Owner: logic.
//
// JSON shape (import docs/reference/bolt-monitor-logic.json as-is):
// { "state_select": "<optional>",
//   "states": { "<name>": { "entry_conditions": [ {"type":"soc","operator":">=","value":80} ],
//                           "charging_logic":  [ {"soc":70,"price_on":-1,"price_off":"min","known_minutes":30} ] } } }
// State order = JSON object order and is significant (first matching state wins).
#pragma once
#include <ArduinoJson.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace logic {

enum class Op : uint8_t { Gt, Ge, Lt, Le };   // ">" ">=" "<" "<=" — honored literally (NOT the PHP bug)
const char* opName(Op op);
bool opFromName(const char* s, Op& out);

struct Cond {             // {"type":"soc", "operator":..., "value":...}; only type "soc" exists
  Op op = Op::Ge;
  float value = 0;        // 0..100
  bool holds(float soc) const;
};

struct PriceRef {         // number, or the string "min"
  bool is_min = false;
  float value = 0;
  float resolve(float min_price) const;  // is_min ? min_price (may be NAN) : value
};

struct Block {
  float soc = 0;              // block applies when soc_now >= soc
  PriceRef on, off;
  uint8_t known_minutes = 0;  // 0 = absent; serialized only when > 0
};

struct State {
  std::string name;               // UTF-8
  std::vector<Cond> entry;        // empty => never auto-entered (only via state_select)
  std::vector<Block> blocks;      // "charging_logic"
  bool has_blocks_key = false;    // charging_logic key present (missing => fall back to states.default)
};

struct LogicConfig {
  std::string state_select;       // "" = auto
  std::vector<State> states;      // config order
  const State* find(const std::string& name) const;  // nullptr if absent
};

// Parse + validate. On failure returns false and appends human-readable messages to `errors`
// (same wording style as the prototype's validateLogic()). `out` is untouched on failure.
bool fromJson(JsonVariantConst in, LogicConfig& out, std::vector<std::string>* errors);
bool parse(const char* json, size_t len, LogicConfig& out, std::vector<std::string>* errors);

// Serialize back to the same shape (state order preserved; known_minutes omitted when 0;
// "min" emitted as the string "min"; state_select omitted when empty).
void toJson(const LogicConfig& cfg, JsonObject out);
std::string toString(const LogicConfig& cfg);

// Sort every state's blocks by soc descending (stable). Applied on save, like bolt-monitor server.py.
void sortBlocks(LogicConfig& cfg);

// Overlapping entry ranges between pairs of states that have entry conditions, e.g. ">= 80" vs "<= 80".
// One message per overlapping pair (wording like prototype overlaps()). Empty = fine. Warnings only.
std::vector<std::string> overlapWarnings(const LogicConfig& cfg);

// Factory default = docs/reference/bolt-monitor-logic.json (Разряд / Заряд / Поездка).
LogicConfig factoryDefault();

}  // namespace logic
