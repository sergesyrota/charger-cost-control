// Decision engine: precedence (PLAN §7.1), overlays (§7.2), price logic (§7.3, port of
// Vehicle::getApplicableLogic + controlCharging with LITERAL operators). Reason strings follow the
// approved UI prototype's decide(). Pure. Owner: logic.
#include "logic/decision_engine.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace logic {

namespace {

void copy(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
}

// Like the prototype fmtC(): round to 0.1, drop ".0", Unicode minus, "¢" suffix. "—" for unknown.
struct C {
  char s[20];
  explicit C(float v) {
    if (isnan(v)) { copy(s, sizeof s, "—"); return; }
    long t = lroundf(v * 10.0f);
    const char* minus = t < 0 ? "−" : "";
    long a = t < 0 ? -t : t;
    if (a % 10 == 0) snprintf(s, sizeof s, "%s%ld¢", minus, a / 10);
    else snprintf(s, sizeof s, "%s%ld.%ld¢", minus, a / 10, a % 10);
  }
};

void setDefault(Decision& d, const DecisionInput& in, const char* why) {
  const bool def = in.cfg ? in.cfg->default_relay : false;
  d.relay = def;
  d.src = DecisionSrc::Default;
  snprintf(d.reason, sizeof d.reason, "%s → relay held at default (%s)", why, def ? "ON" : "OFF");
}

const char* const kDay[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Like prototype winText(): "Mon–Fri 01:00–05:00".
void windowText(char* buf, size_t n, const ScheduleWindow& w) {
  char ds[40] = "";
  const uint8_t d = w.days & 0x7f;
  if (d == 0x3e) copy(ds, sizeof ds, "Mon–Fri");
  else if (d == 0x41) copy(ds, sizeof ds, "Sat–Sun");
  else if (d == 0x7f) copy(ds, sizeof ds, "Every day");
  else {
    size_t len = 0;
    for (int i = 0; i < 7; i++)
      if (d & (1 << i)) len += snprintf(ds + len, sizeof ds - len, "%s%s", len ? " " : "", kDay[i]);
  }
  snprintf(buf, n, "%s %02u:%02u–%02u:%02u", ds, w.start_min / 60, w.start_min % 60, w.end_min / 60,
           w.end_min % 60);
}

}  // namespace

bool overrideExpired(const Override& o, bool clock_valid, int64_t now) {
  return clock_valid && o.until != 0 && now >= o.until;
}

namespace {
bool windowActive(const ScheduleWindow& w, const ChicagoTime& now) {
  const uint16_t m = now.minuteOfDay();
  const uint8_t today = 1u << (now.wday % 7), yesterday = 1u << ((now.wday + 6) % 7);
  if (w.start_min < w.end_min) return (w.days & today) && m >= w.start_min && m < w.end_min;
  if (w.start_min > w.end_min) return ((w.days & today) && m >= w.start_min) || ((w.days & yesterday) && m < w.end_min);
  return false;
}
}  // namespace

const ScheduleWindow* activeWindow(const Config& cfg, const ChicagoTime& now) {
  for (uint8_t i = 0; i < cfg.window_count && i < kMaxWindows; i++)
    if (windowActive(cfg.windows[i], now)) return &cfg.windows[i];
  return nullptr;
}

StatePick pickState(const LogicConfig& cfg, float soc, const char* sticky_state) {
  StatePick p;
  const State* st = nullptr;
  if (!cfg.state_select.empty() && (st = cfg.find(cfg.state_select))) {
    p.how = StateHow::Forced;
  } else {
    st = nullptr;
    for (const State& s : cfg.states) {
      if (s.entry.empty()) continue;  // never auto-entered
      bool all = true;
      for (const Cond& c : s.entry)
        if (!c.holds(soc)) { all = false; break; }
      if (all) { st = &s; p.how = StateHow::Entered; break; }
    }
    if (!st) {
      p.how = StateHow::Sticky;
      st = cfg.find(sticky_state ? sticky_state : "default");
      p.name = sticky_state ? sticky_state : "default";
      // Sticky name doesn't exist (fresh device: "default"; or the state was renamed/removed) and
      // there's no states.default to fall back to: use the first state that has charging bands
      // instead of holding the relay at default until SOC crosses an entry threshold.
      if (!st && !cfg.find("default"))
        for (const State& s : cfg.states)
          if (!s.blocks.empty()) { st = &s; break; }
    }
  }
  p.state = st;
  if (st) p.name = st->name;

  if (st && !st->blocks.empty()) {
    p.blocks = &st->blocks;
  } else if (const State* def = cfg.find("default")) {  // PHP: empty($chargingLogic) -> states.default
    p.blocks = &def->blocks;
    p.fallback = true;
  }
  if (p.blocks && !p.blocks->empty()) {
    const auto& b = *p.blocks;
    p.block_index = (int)b.size() - 1;
    for (size_t i = 0; i < b.size(); i++)
      if (soc >= b[i].soc) { p.block_index = (int)i; break; }
  }
  return p;
}

Decision decide(const DecisionInput& in) {
  Decision d;
  d.at = in.clock_valid ? in.now.epoch : 0;
  copy(d.state, sizeof d.state, in.sticky_state ? in.sticky_state : "default");
  static const Config kDefCfg;
  const Config& cfg = in.cfg ? *in.cfg : kDefCfg;

  // 1. Manual (an expired timed override counts as Auto).
  Mode mode = in.ovr.mode;
  const bool manual = mode == Mode::ManualOn || mode == Mode::ManualOff;
  if (manual && overrideExpired(in.ovr, in.clock_valid, in.now.epoch)) mode = Mode::Auto;
  if (mode == Mode::ManualOn || mode == Mode::ManualOff) {
    const bool on = mode == Mode::ManualOn;
    d.relay = on;
    d.src = DecisionSrc::Manual;
    if (in.ovr.until) {
      ChicagoTime u = chicagoFromEpoch(in.ovr.until);
      snprintf(d.reason, sizeof d.reason, "Manual %s until %02u:%02u", on ? "ON" : "OFF", u.hour, u.minute);
    } else {
      snprintf(d.reason, sizeof d.reason, "Manual %s — indefinitely", on ? "ON" : "OFF");
    }
    return d;
  }

  // 2. Overlays (SOC known and fresh only: a stale 100% must not lock charging out).
  if (!isnan(in.soc) && in.soc_fresh) {
    if (lroundf(in.soc) >= 100) {
      const uint32_t hold = cfg.full_hold_min;
      const bool held = hold == 0 || !in.clock_valid || in.full_since == 0 ||
                        in.now.epoch - in.full_since >= (int64_t)hold * 60;
      if (held) {
        d.relay = false;
        d.src = DecisionSrc::Overlay;
        copy(d.reason, sizeof d.reason, "Bolt is full");
        return d;
      }
    }
    if (cfg.soc_target_enabled && in.soc >= cfg.soc_target) {
      d.relay = false;
      d.src = DecisionSrc::Overlay;
      snprintf(d.reason, sizeof d.reason, "SOC target %u%% reached", cfg.soc_target);
      return d;
    }
  }

  // 3. Schedule.
  if (mode == Mode::Schedule || mode == Mode::SchedulePrice) {
    if (!in.clock_valid) { setDefault(d, in, "Clock not set yet, schedule can’t run"); return d; }
    // An uncapped active window, or a capped one with a known price <= cap, forces ON. A capped
    // window over its cap (or with unknown price) behaves like "outside the windows".
    const ScheduleWindow* capped = nullptr;
    for (uint8_t i = 0; i < cfg.window_count && i < kMaxWindows; i++) {
      const ScheduleWindow& w = cfg.windows[i];
      if (!windowActive(w, in.now)) continue;
      const bool has_cap = !isnan(w.max_price);
      if (has_cap && (isnan(in.price) || in.price > w.max_price)) {
        if (!capped) capped = &w;
        continue;
      }
      char wt[64];
      windowText(wt, sizeof wt, w);
      d.relay = true;
      d.src = DecisionSrc::Schedule;
      d.price = in.price;
      if (has_cap)
        snprintf(d.reason, sizeof d.reason, "Inside schedule window %s (%s ≤ %s cap)", wt, C(in.price).s,
                 C(w.max_price).s);
      else
        snprintf(d.reason, sizeof d.reason, "Inside schedule window %s", wt);
      return d;
    }
    if (mode == Mode::Schedule) {
      d.relay = false;
      d.src = DecisionSrc::Schedule;
      if (capped && isnan(in.price))
        snprintf(d.reason, sizeof d.reason, "Schedule window capped at %s, price unknown", C(capped->max_price).s);
      else if (capped)
        snprintf(d.reason, sizeof d.reason, "Schedule window price cap exceeded (%s > %s)", C(in.price).s,
                 C(capped->max_price).s);
      else
        copy(d.reason, sizeof d.reason, "Outside schedule windows (schedule only)");
      return d;
    }
  }

  // 4. Price logic.
  if (!in.clock_valid) { setDefault(d, in, "Clock not set yet (waiting for NTP)"); return d; }
  if (isnan(in.price)) { setDefault(d, in, "No fresh price data"); return d; }
  if (!in.logic) { setDefault(d, in, "No price logic configured"); return d; }

  const float soc = isnan(in.soc) ? 0.0f : in.soc;
  StatePick p = pickState(*in.logic, soc, in.sticky_state);
  // New sticky state: changes only when a state was entered or forced (PHP setCache).
  // (A sticky name that didn't exist and was resolved to a real state is persisted too.)
  if (p.state) copy(d.state, sizeof d.state, p.name.c_str());
  if (p.block_index < 0) {
    char why[120];
    snprintf(why, sizeof why, "State “%s” has no charging bands", p.name.c_str());
    setDefault(d, in, why);
    return d;
  }
  const Block& b = (*p.blocks)[p.block_index];
  const float on = b.on.resolve(in.min_price), off = b.off.resolve(in.min_price);
  if (isnan(on) || isnan(off)) { setDefault(d, in, "“min” price unavailable"); return d; }

  d.src = DecisionSrc::Price;
  d.how = p.how;
  d.block_index = (int8_t)p.block_index;
  d.block_soc = b.soc;
  d.logic_fallback = p.fallback;
  d.price = in.price;
  d.price_on = on;
  d.price_off = off;
  d.known_minutes = b.known_minutes;

  const uint8_t minute = in.now.minute, km = b.known_minutes;
  const bool enough = km == 0 || minute >= km;
  const float pr = in.price;
  d.relay = in.relay_now;
  if (in.relay_now) {
    if (pr > off) {
      d.relay = false;
      snprintf(d.reason, sizeof d.reason, "Price is high (%s > %s)", C(pr).s, C(off).s);
    } else if (!enough) {
      d.relay = false;
      snprintf(d.reason, sizeof d.reason, "Not enough confidence yet (:%02u < :%02u)", minute, km);
    } else {
      snprintf(d.reason, sizeof d.reason, "Price %s is at or below stop %s — keep charging", C(pr).s, C(off).s);
    }
  } else {
    if (pr <= on && enough) {
      d.relay = true;
      snprintf(d.reason, sizeof d.reason, "Price is low (%s ≤ %s)", C(pr).s, C(on).s);
    } else if (pr <= on) {
      snprintf(d.reason, sizeof d.reason, "Price is low, waiting for :%02u confidence", km);
    } else {
      snprintf(d.reason, sizeof d.reason, "Price %s is above start %s — waiting", C(pr).s, C(on).s);
    }
  }
  return d;
}

}  // namespace logic
