// Decision engine: precedence (PLAN §7.1), overlays (§7.2), price logic (§7.3, port of
// Vehicle::getApplicableLogic + controlCharging with LITERAL operators). Reason strings follow the
// approved UI prototype's decide(). Pure. Owner: logic.
#include "logic/decision_engine.h"

#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "util/utf8.h"

namespace logic {

namespace {

void copy(char* dst, size_t n, const char* src) {
  strncpy(dst, src ? src : "", n - 1);
  dst[n - 1] = 0;
  utf8FixTail(dst);
}

// d.reason = printf(...), never ending in half a UTF-8 character.
void say(Decision& d, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void say(Decision& d, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(d.reason, sizeof d.reason, fmt, ap);
  va_end(ap);
  utf8FixTail(d.reason);
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
  say(d, "%s → реле по умолчанию (%s)", why, def ? "ВКЛ" : "ВЫКЛ");
}

const char* const kDay[] = {"Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб"};

// Like the UI winText(): "Пн–Пт 01:00–05:00" (week starts on Monday).
void windowText(char* buf, size_t n, const ScheduleWindow& w) {
  char ds[40] = "";
  const uint8_t d = w.days & 0x7f;
  if (d == 0x3e) copy(ds, sizeof ds, "Пн–Пт");
  else if (d == 0x41) copy(ds, sizeof ds, "Сб–Вс");
  else if (d == 0x7f) copy(ds, sizeof ds, "Ежедневно");
  else {
    size_t len = 0;
    for (int k = 1; k <= 7; k++) {  // Monday first
      const int i = k % 7;
      if (d & (1 << i)) len += snprintf(ds + len, sizeof ds - len, "%s%s", len ? " " : "", kDay[i]);
    }
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
  // A timed override restored after a reboot is unknown until the clock is set (it may have ended
  // while the board was off): until then -> Auto, i.e. the default relay.
  if (manual && in.ovr.until && !in.clock_valid) mode = Mode::Auto;
  if (mode == Mode::ManualOn || mode == Mode::ManualOff) {
    const bool on = mode == Mode::ManualOn;
    d.relay = on;
    d.src = DecisionSrc::Manual;
    if (in.ovr.until) {
      ChicagoTime u = chicagoFromEpoch(in.ovr.until);
      say(d, "Ручное %s до %02u:%02u", on ? "ВКЛ" : "ВЫКЛ", u.hour, u.minute);
    } else {
      say(d, "Ручное %s — бессрочно", on ? "ВКЛ" : "ВЫКЛ");
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
        copy(d.reason, sizeof d.reason, "Bolt заряжен полностью");
        return d;
      }
    }
    if (cfg.soc_target_enabled && in.soc >= cfg.soc_target) {
      d.relay = false;
      d.src = DecisionSrc::Overlay;
      say(d, "Достигнут целевой заряд %u%%", cfg.soc_target);
      return d;
    }
  }

  // 3. Schedule.
  if (mode == Mode::Schedule || mode == Mode::SchedulePrice) {
    if (!in.clock_valid) { setDefault(d, in, "Часы ещё не установлены, расписание не работает"); return d; }
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
        say(d, "Окно расписания %s (%s ≤ лимит %s)", wt, C(in.price).s, C(w.max_price).s);
      else
        say(d, "Окно расписания %s", wt);
      return d;
    }
    if (mode == Mode::Schedule) {
      d.relay = false;
      d.src = DecisionSrc::Schedule;
      if (capped && isnan(in.price))
        say(d, "Окно расписания с лимитом %s, цена неизвестна", C(capped->max_price).s);
      else if (capped)
        say(d, "Цена выше лимита окна расписания (%s > %s)", C(in.price).s, C(capped->max_price).s);
      else
        copy(d.reason, sizeof d.reason, "Вне окон расписания (только расписание)");
      return d;
    }
  }

  // 4. Price logic.
  if (!in.clock_valid) { setDefault(d, in, "Часы ещё не установлены (ждём NTP)"); return d; }
  if (isnan(in.price)) { setDefault(d, in, "Нет свежих данных о цене"); return d; }
  if (!in.logic) { setDefault(d, in, "Ценовые правила не заданы"); return d; }

  const float soc = isnan(in.soc) ? 0.0f : in.soc;
  StatePick p = pickState(*in.logic, soc, in.sticky_state);
  // New sticky state: changes only when a state was entered or forced (PHP setCache).
  // (A sticky name that didn't exist and was resolved to a real state is persisted too.)
  if (p.state) copy(d.state, sizeof d.state, p.name.c_str());
  if (p.block_index < 0) {
    char why[160];
    snprintf(why, sizeof why, "У состояния «%s» нет диапазонов заряда", p.name.c_str());
    utf8FixTail(why);
    setDefault(d, in, why);
    return d;
  }
  const Block& b = (*p.blocks)[p.block_index];
  const float on = b.on.resolve(in.min_price), off = b.off.resolve(in.min_price);
  if (isnan(on) || isnan(off)) { setDefault(d, in, "Цена «min» недоступна"); return d; }

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
      say(d, "Цена высокая (%s > %s)", C(pr).s, C(off).s);
    } else if (!enough) {
      d.relay = false;
      say(d, "Мало данных за час (:%02u < :%02u)", minute, km);
    } else {
      say(d, "Цена %s не выше порога остановки %s — заряжаем дальше", C(pr).s, C(off).s);
    }
  } else {
    if (pr <= on && enough) {
      d.relay = true;
      say(d, "Цена низкая (%s ≤ %s)", C(pr).s, C(on).s);
    } else if (pr <= on) {
      say(d, "Цена низкая, ждём :%02u для надёжной цены", km);
    } else {
      say(d, "Цена %s выше порога старта %s — ждём", C(pr).s, C(on).s);
    }
  }
  return d;
}

}  // namespace logic
