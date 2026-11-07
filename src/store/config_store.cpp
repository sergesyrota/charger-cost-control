// STUB — owner: store.
#include "store/config_store.h"

namespace cfgstore {
void begin() {}
bool loadConfig(Config& out) { out = Config(); return false; }
bool saveConfig(const Config&) { return false; }
bool loadLogic(logic::LogicConfig& out) { out = logic::factoryDefault(); return false; }
bool saveLogic(const logic::LogicConfig&) { return false; }
void factoryReset() {}
}  // namespace cfgstore
