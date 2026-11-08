// logic.json parse / validate / serialize / overlap warnings. Pure. Owner: logic.
#include "logic/logic_config.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>

#include "core_types.h"

namespace logic {

static constexpr size_t kMaxStates = 16;
static constexpr size_t kMaxBlocks = 16;
static constexpr size_t kMaxConds = 8;

const char* opName(Op op) {
  static const char* const n[] = {">", ">=", "<", "<="};
  return n[(int)op];
}

bool opFromName(const char* s, Op& out) {
  if (!s) return false;
  for (int i = 0; i < 4; i++)
    if (strcmp(s, opName((Op)i)) == 0) { out = (Op)i; return true; }
  return false;
}

bool Cond::holds(float soc) const {
  switch (op) {
    case Op::Gt: return soc > value;
    case Op::Ge: return soc >= value;
    case Op::Lt: return soc < value;
    case Op::Le: return soc <= value;
  }
  return false;
}

float PriceRef::resolve(float min_price) const { return is_min ? min_price : value; }

const State* LogicConfig::find(const std::string& name) const {
  for (const auto& s : states)
    if (s.name == name) return &s;
  return nullptr;
}

// ------------------------------------------------------------------ parse + validate

static void err(std::vector<std::string>* errors, const std::string& name, const char* fmt, int i,
                const char* extra = nullptr) {
  if (!errors) return;
  char buf[192];
  if (extra) snprintf(buf, sizeof buf, fmt, i, extra);
  else snprintf(buf, sizeof buf, fmt, i);
  errors->push_back(name + ": " + buf);
}

static bool isNum(JsonVariantConst v) { return v.is<float>() && !v.is<bool>(); }

static bool parsePrice(JsonVariantConst v, PriceRef& out) {
  if (v.is<const char*>() && strcmp(v.as<const char*>(), "min") == 0) {
    out.is_min = true;
    out.value = 0;
    return true;
  }
  if (!isNum(v)) return false;
  out.is_min = false;
  out.value = v.as<float>();
  return true;
}

bool fromJson(JsonVariantConst in, LogicConfig& out, std::vector<std::string>* errors) {
  JsonObjectConst root = in.as<JsonObjectConst>();
  JsonObjectConst states = root.isNull() ? JsonObjectConst() : root["states"].as<JsonObjectConst>();
  if (states.isNull()) {
    if (errors) errors->push_back("Нет объекта \"states\"");
    return false;
  }
  bool ok = true;
  auto fail = [&]() { ok = false; };

  LogicConfig cfg;
  if (states.size() == 0) {
    if (errors) errors->push_back("Нужно хотя бы одно состояние");
    fail();
  }
  if (states.size() > kMaxStates) {
    if (errors) errors->push_back("Слишком много состояний (максимум 16)");
    fail();
  }
  for (JsonPairConst kv : states) {
    State st;
    st.name = kv.key().c_str();
    const std::string& n = st.name;
    if (st.name.empty()) { err(errors, n, "имя состояния не может быть пустым", 0); fail(); }
    if (st.name.size() >= kStateNameMax) { err(errors, n, "имя слишком длинное (максимум %d байт)", (int)kStateNameMax - 1); fail(); }
    if (cfg.find(st.name)) { err(errors, n, "состояние повторяется", 0); fail(); continue; }
    JsonObjectConst so = kv.value().as<JsonObjectConst>();
    if (so.isNull()) { err(errors, n, "должно быть объектом", 0); fail(); continue; }

    JsonVariantConst cs = so["entry_conditions"];
    if (!cs.isUnbound() && !cs.isNull()) {
      JsonArrayConst ca = cs.as<JsonArrayConst>();
      if (ca.isNull()) { err(errors, n, "entry_conditions должен быть списком", 0); fail(); }
      else {
        if (ca.size() > kMaxConds) { err(errors, n, "слишком много условий (максимум %d)", (int)kMaxConds); fail(); }
        int i = 0;
        for (JsonVariantConst c : ca) {
          ++i;
          Cond cond;
          const char* type = c["type"].as<const char*>();
          if (!type || strcmp(type, "soc") != 0) { err(errors, n, "условие %d: type должен быть \"soc\"", i); fail(); }
          if (!opFromName(c["operator"].as<const char*>(), cond.op)) {
            err(errors, n, "условие %d: operator должен быть одним из > >= < <=", i); fail();
          }
          JsonVariantConst v = c["value"];
          if (!isNum(v) || v.as<float>() < 0 || v.as<float>() > 100) {
            err(errors, n, "условие %d: value должно быть 0–100", i); fail();
          } else cond.value = v.as<float>();
          st.entry.push_back(cond);
        }
      }
    }

    JsonVariantConst bl = so["charging_logic"];
    if (!bl.isUnbound() && !bl.isNull()) {
      JsonArrayConst ba = bl.as<JsonArrayConst>();
      if (ba.isNull()) { err(errors, n, "charging_logic должен быть списком", 0); fail(); }
      else {
        st.has_blocks_key = true;
        if (ba.size() > kMaxBlocks) { err(errors, n, "слишком много диапазонов (максимум %d)", (int)kMaxBlocks); fail(); }
        int i = 0;
        for (JsonVariantConst b : ba) {
          ++i;
          Block blk;
          JsonVariantConst s = b["soc"];
          if (!isNum(s) || s.as<float>() < 0 || s.as<float>() > 100) {
            err(errors, n, "диапазон %d: soc должен быть 0–100", i); fail();
          } else blk.soc = s.as<float>();
          if (!parsePrice(b["price_on"], blk.on)) { err(errors, n, "диапазон %d: %s должен быть числом или \"min\"", i, "price_on"); fail(); }
          if (!parsePrice(b["price_off"], blk.off)) { err(errors, n, "диапазон %d: %s должен быть числом или \"min\"", i, "price_off"); fail(); }
          JsonVariantConst km = b["known_minutes"];
          if (!km.isUnbound() && !km.isNull()) {
            if (!km.is<int>() || km.as<int>() < 0 || km.as<int>() > 59) {
              err(errors, n, "диапазон %d: known_minutes должен быть 0–59", i); fail();
            } else blk.known_minutes = (uint8_t)km.as<int>();
          }
          st.blocks.push_back(blk);
        }
      }
    }
    cfg.states.push_back(std::move(st));
  }

  JsonVariantConst sel = root["state_select"];
  if (!sel.isUnbound() && !sel.isNull()) {
    const char* s = sel.as<const char*>();
    if (!s) {
      if (errors) errors->push_back("state_select должен быть строкой");
      fail();
    } else if (*s) {
      if (!cfg.find(s)) {
        if (errors) errors->push_back(std::string("state_select: состояния «") + s + "» нет");
        fail();
      } else cfg.state_select = s;
    }
  }
  if (!ok) return false;
  out = std::move(cfg);
  return true;
}

bool parse(const char* json, size_t len, LogicConfig& out, std::vector<std::string>* errors) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len)) {
    if (errors) errors->push_back("некорректный JSON");
    return false;
  }
  return fromJson(doc.as<JsonVariantConst>(), out, errors);
}

// ------------------------------------------------------------------ serialize

static void putNum(JsonObject o, const char* key, float f) {
  if (f == floorf(f) && fabsf(f) < 1e6f) o[key] = (long)f;
  else o[key] = f;
}

static void putPrice(JsonObject o, const char* key, const PriceRef& p) {
  if (p.is_min) o[key] = "min";
  else putNum(o, key, p.value);
}

void toJson(const LogicConfig& cfg, JsonObject out) {
  if (!cfg.state_select.empty()) out["state_select"] = cfg.state_select.c_str();
  JsonObject states = out["states"].to<JsonObject>();
  for (const State& st : cfg.states) {
    JsonObject so = states[st.name.c_str()].to<JsonObject>();
    JsonArray ca = so["entry_conditions"].to<JsonArray>();
    for (const Cond& c : st.entry) {
      JsonObject co = ca.add<JsonObject>();
      co["type"] = "soc";
      co["operator"] = opName(c.op);
      putNum(co, "value", c.value);
    }
    if (st.has_blocks_key || !st.blocks.empty()) {
      JsonArray ba = so["charging_logic"].to<JsonArray>();
      for (const Block& b : st.blocks) {
        JsonObject bo = ba.add<JsonObject>();
        putNum(bo, "soc", b.soc);
        putPrice(bo, "price_on", b.on);
        putPrice(bo, "price_off", b.off);
        if (b.known_minutes > 0) bo["known_minutes"] = b.known_minutes;
      }
    }
  }
}

std::string toString(const LogicConfig& cfg) {
  JsonDocument doc;
  toJson(cfg, doc.to<JsonObject>());
  std::string s;
  serializeJson(doc, s);
  return s;
}

void sortBlocks(LogicConfig& cfg) {
  for (State& st : cfg.states)
    std::stable_sort(st.blocks.begin(), st.blocks.end(),
                     [](const Block& a, const Block& b) { return a.soc > b.soc; });
}

// ------------------------------------------------------------------ overlaps (port of prototype overlaps())

namespace {
struct Iv {
  float lo = 0, hi = 100;
  bool li = true, hi_ = true;
};
Iv interval(const std::vector<Cond>& cs) {
  Iv r;
  for (const Cond& c : cs) {
    const float v = c.value;
    if (c.op == Op::Gt || c.op == Op::Ge) {
      const bool inc = c.op == Op::Ge;
      if (v > r.lo || (v == r.lo && !inc)) { r.lo = v; r.li = inc; }
    } else {
      const bool inc = c.op == Op::Le;
      if (v < r.hi || (v == r.hi && !inc)) { r.hi = v; r.hi_ = inc; }
    }
  }
  return r;
}
bool nonEmpty(const Iv& a) { return a.lo < a.hi || (a.lo == a.hi && a.li && a.hi_); }
void fmtNum(char* buf, size_t n, float v) {
  if (v == floorf(v)) snprintf(buf, n, "%d", (int)v);
  else snprintf(buf, n, "%g", (double)v);
}
}  // namespace

std::vector<std::string> overlapWarnings(const LogicConfig& cfg) {
  std::vector<std::string> out;
  std::vector<const State*> st;
  for (const State& s : cfg.states)
    if (!s.entry.empty()) st.push_back(&s);
  for (size_t i = 0; i < st.size(); i++)
    for (size_t j = i + 1; j < st.size(); j++) {
      Iv a = interval(st[i]->entry), b = interval(st[j]->entry), x = a;
      if (b.lo > x.lo || (b.lo == x.lo && !b.li)) { x.lo = b.lo; x.li = b.li; }
      if (b.hi < x.hi || (b.hi == x.hi && !b.hi_)) { x.hi = b.hi; x.hi_ = b.hi_; }
      if (!(nonEmpty(a) && nonEmpty(b) && nonEmpty(x))) continue;
      char lo[16], hi[16], range[48];
      fmtNum(lo, sizeof lo, x.lo);
      fmtNum(hi, sizeof hi, x.hi);
      if (x.lo == x.hi) snprintf(range, sizeof range, "%s%%", lo);
      else snprintf(range, sizeof range, "%s%s–%s%s%%", x.li ? "[" : "(", lo, hi, x.hi_ ? "]" : ")");
      out.push_back("«" + st[i]->name + "» и «" + st[j]->name + "» оба подходят при заряде " + range +
                    " — выигрывает первое по порядку, состояние может переключаться туда-обратно");
    }
  return out;
}

// ------------------------------------------------------------------ factory default

static const char kFactoryJson[] =
    R"({"states":{)"
    R"("Разряд":{"entry_conditions":[{"type":"soc","value":80,"operator":">="}],"charging_logic":[)"
    R"({"soc":70,"price_on":-1,"price_off":-0.5,"known_minutes":30},)"
    R"({"soc":20,"price_on":-0.5,"price_off":0,"known_minutes":20},)"
    R"({"soc":0,"price_on":2,"price_off":3}]},)"
    R"("Заряд":{"entry_conditions":[{"type":"soc","value":20,"operator":"<="}],"charging_logic":[)"
    R"({"soc":80,"price_on":-1,"price_off":-0.5,"known_minutes":20},)"
    R"({"soc":40,"price_on":0,"price_off":0.5,"known_minutes":20},)"
    R"({"soc":0,"price_on":2,"price_off":3}]},)"
    R"("Поездка":{"entry_conditions":[],"charging_logic":[{"soc":0,"price_on":3,"price_off":3}]})"
    R"(}})";

LogicConfig factoryDefault() {
  LogicConfig c;
  parse(kFactoryJson, sizeof(kFactoryJson) - 1, c, nullptr);
  return c;
}

}  // namespace logic
