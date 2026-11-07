// NVS persistence of the user config and the price-logic JSON (PLAN §9.2). Owner: store.
// Namespace "cc": key "cfg" = cfgjson::toStorage(Config), key "logic" = logic::toString().
// Validate on load; on error fall back to factory defaults and log it (event_log).
#pragma once
#include "core_types.h"
#include "logic/logic_config.h"

namespace cfgstore {

void begin();
bool loadConfig(Config& out);                  // false => factory defaults written to `out`
bool saveConfig(const Config& c);
bool loadLogic(logic::LogicConfig& out);       // false => logic::factoryDefault()
bool saveLogic(const logic::LogicConfig& l);
void factoryReset();                           // erase cfg, logic, state (not Wi-Fi)

}  // namespace cfgstore
