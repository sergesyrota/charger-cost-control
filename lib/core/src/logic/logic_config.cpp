// STUB — owner: logic.
#include "logic/logic_config.h"

namespace logic {

const char* opName(Op op) {
  static const char* const n[] = {">", ">=", "<", "<="};
  return n[(int)op];
}
bool opFromName(const char*, Op&) { return false; }
bool Cond::holds(float) const { return false; }
float PriceRef::resolve(float min_price) const { return is_min ? min_price : value; }

const State* LogicConfig::find(const std::string& name) const {
  for (const auto& s : states)
    if (s.name == name) return &s;
  return nullptr;
}

bool fromJson(JsonVariantConst, LogicConfig&, std::vector<std::string>* errors) {
  if (errors) errors->push_back("not implemented");
  return false;
}
bool parse(const char* json, size_t len, LogicConfig& out, std::vector<std::string>* errors) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) {
    if (errors) errors->push_back("invalid JSON");
    return false;
  }
  return fromJson(doc.as<JsonVariantConst>(), out, errors);
}
void toJson(const LogicConfig&, JsonObject) {}
std::string toString(const LogicConfig& cfg) {
  JsonDocument doc;
  toJson(cfg, doc.to<JsonObject>());
  std::string s;
  serializeJson(doc, s);
  return s;
}
void sortBlocks(LogicConfig&) {}
std::vector<std::string> overlapWarnings(const LogicConfig&) { return {}; }
LogicConfig factoryDefault() { return LogicConfig(); }

}  // namespace logic
