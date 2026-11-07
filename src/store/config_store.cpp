// NVS persistence for config + price logic. Owner: store.
#include "store/config_store.h"

#include <Preferences.h>

#include <string>

#include "store/config_codec.h"
#include "store/event_log.h"

namespace cfgstore {

namespace {
constexpr const char* kNs = "cc";

// Returns false if the key is missing/empty.
bool readBlob(const char* key, std::string& out) {
  Preferences p;
  if (!p.begin(kNs, true)) return false;
  size_t n = p.getBytesLength(key);
  bool ok = false;
  if (n) {
    out.assign(n, '\0');
    ok = p.getBytes(key, &out[0], n) == n;
  }
  p.end();
  return ok;
}

bool writeBlob(const char* key, const std::string& s) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  bool ok = p.putBytes(key, s.data(), s.size()) == s.size();
  p.end();
  return ok;
}
}  // namespace

void begin() {}

bool loadConfig(Config& out) {
  std::string s;
  if (!readBlob("cfg", s)) { out = Config(); return false; }   // first boot: factory defaults
  if (cfgjson::fromStorage(s.data(), s.size(), out)) return true;
  out = Config();
  evlog::add(evlog::Kind::Err, "Stored config invalid; using factory defaults");
  return false;
}

bool saveConfig(const Config& c) { return writeBlob("cfg", cfgjson::toStorage(c)); }

bool loadLogic(logic::LogicConfig& out) {
  std::string s;
  if (!readBlob("logic", s)) { out = logic::factoryDefault(); return false; }
  logic::LogicConfig t;
  if (logic::parse(s.data(), s.size(), t, nullptr)) { out = t; return true; }
  out = logic::factoryDefault();
  evlog::add(evlog::Kind::Err, "Stored price logic invalid; using factory defaults");
  return false;
}

bool saveLogic(const logic::LogicConfig& l) { return writeBlob("logic", logic::toString(l)); }

void factoryReset() {
  Preferences p;
  if (p.begin(kNs, false)) {   // Wi-Fi credentials live in the Wi-Fi library's own namespace
    p.clear();
    p.end();
  }
}

}  // namespace cfgstore
