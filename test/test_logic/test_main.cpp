// Owner: logic. Sparse, high-value only (decision table vs PHP semantics, boundaries, overlaps).
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>

#include "logic/decision_engine.h"

using namespace logic;

static const char* RAZ = "Разряд";
static const char* ZAR = "Заряд";
static const char* POE = "Поездка";

static LogicConfig g_ref;
static Config g_cfg;

void setUp() { g_cfg = Config(); }
void tearDown() {}

// 2026-11-07 22:35 CST (Saturday) + `min` minutes.
static ChicagoTime at(int min = 35) { return chicagoFromEpoch(1794112500 + (min - 35) * 60); }

static Decision run(float price, float soc, bool relay, const char* sticky, int minute = 35,
                    const LogicConfig* l = &g_ref, float min_price = NAN) {
  DecisionInput in;
  in.cfg = &g_cfg;
  in.logic = l;
  in.clock_valid = true;
  in.now = at(minute);
  in.price = price;
  in.min_price = min_price;
  in.soc = soc;
  in.relay_now = relay;
  in.sticky_state = sticky;
  return decide(in);
}

static void test_reference_parse_and_roundtrip() {
  FILE* f = fopen("docs/reference/bolt-monitor-logic.json", "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, "run from project root");
  char buf[4096];
  size_t n = fread(buf, 1, sizeof buf, f);
  fclose(f);
  std::vector<std::string> errs;
  TEST_ASSERT_TRUE(parse(buf, n, g_ref, &errs));
  TEST_ASSERT_EQUAL(3, g_ref.states.size());
  TEST_ASSERT_EQUAL_STRING(RAZ, g_ref.states[0].name.c_str());   // \u escapes -> UTF-8, order kept
  TEST_ASSERT_EQUAL_STRING(POE, g_ref.states[2].name.c_str());
  TEST_ASSERT_EQUAL(30, g_ref.states[0].blocks[0].known_minutes);
  TEST_ASSERT_EQUAL_STRING(toString(g_ref).c_str(), toString(factoryDefault()).c_str());
  LogicConfig back;
  std::string s = toString(g_ref);
  errs.clear();
  bool ok = parse(s.c_str(), s.size(), back, &errs);
  TEST_ASSERT_TRUE_MESSAGE(ok, (s + (errs.empty() ? "" : errs[0])).c_str());
  TEST_ASSERT_EQUAL_STRING(s.c_str(), toString(back).c_str());
  TEST_ASSERT_EQUAL(0, overlapWarnings(g_ref).size());   // >=80 vs <=20 don't overlap
}

// Hysteresis: between 20 and 80 the sticky state is kept; at the exact boundaries it's entered.
static void test_stickiness() {
  struct { float soc; const char* sticky; const char* want; StateHow how; } t[] = {
      {50, ZAR, ZAR, StateHow::Sticky}, {50, RAZ, RAZ, StateHow::Sticky},
      {80, ZAR, RAZ, StateHow::Entered}, {79.9f, ZAR, ZAR, StateHow::Sticky},
      {20, RAZ, ZAR, StateHow::Entered}, {20.1f, RAZ, RAZ, StateHow::Sticky},
      {50, POE, POE, StateHow::Sticky},  // empty entry_conditions: kept once forced/sticky
  };
  for (auto& c : t) {
    StatePick p = pickState(g_ref, c.soc, c.sticky);
    TEST_ASSERT_EQUAL_STRING(c.want, p.name.c_str());
    TEST_ASSERT_EQUAL((int)c.how, (int)p.how);
    Decision d = run(1, c.soc, false, c.sticky);
    TEST_ASSERT_EQUAL_STRING(c.want, d.state);   // persisted sticky state
  }
}

static void test_operators_literal_and_overlap() {
  const char* j = R"({"states":{"A":{"entry_conditions":[{"type":"soc","operator":">","value":80}],
    "charging_logic":[{"soc":0,"price_on":1,"price_off":2}]},
    "B":{"entry_conditions":[{"type":"soc","operator":"<=","value":80}]},
    "C":{"entry_conditions":[{"type":"soc","operator":">=","value":80}]}}})";
  LogicConfig l;
  TEST_ASSERT_TRUE(parse(j, strlen(j), l, nullptr));
  TEST_ASSERT_EQUAL_STRING("B", pickState(l, 80, "x").name.c_str());     // '>' strict: A not at 80
  TEST_ASSERT_EQUAL_STRING("A", pickState(l, 80.5f, "x").name.c_str());
  // A(>80)/B(<=80) are disjoint; B/C share exactly 80; A/C share (80–100].
  auto w = overlapWarnings(l);
  TEST_ASSERT_EQUAL(2, w.size());
  TEST_ASSERT_EQUAL_STRING("“A” and “C” both match at battery (80–100]% — the first in order wins, the state may flip",
                           w[0].c_str());
  TEST_ASSERT_EQUAL_STRING("“B” and “C” both match at battery 80% — the first in order wins, the state may flip",
                           w[1].c_str());
  // B has no charging_logic and there's no "default" state -> default relay.
  Decision d = run(0, 50, false, "B", 35, &l);
  TEST_ASSERT_EQUAL((int)DecisionSrc::Default, (int)d.src);
  TEST_ASSERT_EQUAL_STRING("State “B” has no charging bands → relay held at default (OFF)", d.reason);
}

static void test_state_select_and_blocks() {
  LogicConfig l = g_ref;
  l.state_select = POE;
  Decision d = run(3, 90, false, ZAR, 35, &l);   // forced beats entry conditions
  TEST_ASSERT_EQUAL_STRING(POE, d.state);
  TEST_ASSERT_EQUAL((int)StateHow::Forced, (int)d.how);
  TEST_ASSERT_TRUE(d.relay);                        // 3 <= 3
  // Block edges in Заряд [80,40,0]: soc >= block.soc picks first; below all -> last.
  struct { float soc; int bi; } t[] = {{100, 0}, {80, 0}, {79.9f, 1}, {40, 1}, {39, 2}, {0, 2}};
  for (auto& c : t) TEST_ASSERT_EQUAL(c.bi, pickState(g_ref, c.soc, ZAR).block_index);
  LogicConfig neg;
  const char* j = R"({"states":{"S":{"entry_conditions":[],"charging_logic":[{"soc":50,"price_on":1,"price_off":2}]}}})";
  TEST_ASSERT_TRUE(parse(j, strlen(j), neg, nullptr));
  TEST_ASSERT_EQUAL(0, pickState(neg, 10, "S").block_index);   // none match -> last
}

// Each transition branch + "min" substitution + known_minutes boundary.
static void test_transitions() {
  // Заряд, soc 50 -> block {40, on 0, off 0.5, km 20}
  struct { float price; bool relay; int minute; bool want; const char* reason; } t[] = {
      {0.6f, true, 35, false, "Price is high (0.6¢ > 0.5¢)"},
      {0.5f, true, 35, true, "Price 0.5¢ is at or below stop 0.5¢ — keep charging"},
      {0.2f, true, 19, false, "Not enough confidence yet (:19 < :20)"},
      {0.0f, false, 20, true, "Price is low (0¢ ≤ 0¢)"},
      {0.0f, false, 19, false, "Price is low, waiting for :20 confidence"},
      {0.1f, false, 35, false, "Price 0.1¢ is above start 0¢ — waiting"},
  };
  for (auto& c : t) {
    Decision d = run(c.price, 50, c.relay, ZAR, c.minute);
    TEST_ASSERT_EQUAL_MESSAGE(c.want, d.relay, c.reason);
    TEST_ASSERT_EQUAL_STRING(c.reason, d.reason);
    TEST_ASSERT_EQUAL((int)DecisionSrc::Price, (int)d.src);
  }
  // "min": {soc 0, on "min", off "min"}; min_price = lowest7 + offset, supplied by price module.
  const char* j = R"({"states":{"default":{"charging_logic":[{"soc":0,"price_on":"min","price_off":"min"}]}}})";
  LogicConfig l;
  TEST_ASSERT_TRUE(parse(j, strlen(j), l, nullptr));
  Decision d = run(-1.2f, NAN, false, "default", 35, &l, -1.1f);
  TEST_ASSERT_TRUE(d.relay);
  TEST_ASSERT_EQUAL_STRING("Price is low (−1.2¢ ≤ −1.1¢)", d.reason);
  d = run(1, NAN, false, "default", 35, &l, NAN);
  TEST_ASSERT_EQUAL_STRING("“min” price unavailable → relay held at default (OFF)", d.reason);
  // Unknown state falls back to states.default's blocks.
  d = run(1, 50, false, "gone", 35, &l, 2);
  TEST_ASSERT_TRUE(d.logic_fallback);
  TEST_ASSERT_TRUE(d.relay);
}

static void test_precedence_and_default() {
  // Unknown price / clock -> default.
  Decision d = run(NAN, 50, true, ZAR);
  TEST_ASSERT_FALSE(d.relay);
  TEST_ASSERT_EQUAL_STRING("No fresh price data → relay held at default (OFF)", d.reason);
  DecisionInput in;
  in.cfg = &g_cfg;
  in.logic = &g_ref;
  in.price = 0;
  TEST_ASSERT_EQUAL_STRING("Clock not set yet (waiting for NTP) → relay held at default (OFF)", decide(in).reason);
  in.ovr.mode = Mode::ManualOn;   // manual works without a clock, overlays don't apply
  in.soc = 100;
  d = decide(in);
  TEST_ASSERT_TRUE(d.relay);
  TEST_ASSERT_EQUAL_STRING("Manual ON — indefinitely", d.reason);
  // Overlays in auto.
  d = run(-5, 100, true, RAZ);
  TEST_ASSERT_FALSE(d.relay);
  TEST_ASSERT_EQUAL_STRING("Bolt is full", d.reason);
  g_cfg.soc_target_enabled = true;
  d = run(-5, 80, true, RAZ);
  TEST_ASSERT_EQUAL_STRING("SOC target 80% reached", d.reason);
}

static void test_schedule_overnight() {
  g_cfg.window_count = 1;
  g_cfg.windows[0] = {0x20 /*Fri*/, 22 * 60, 5 * 60};   // Fri 22:00 -> Sat 05:00
  ChicagoTime t = at();                                   // Sat 22:35
  TEST_ASSERT_EQUAL(6, t.wday);
  TEST_ASSERT_NULL(activeWindow(g_cfg, t));               // Sat not in days
  t.minute = 0; t.hour = 4;                               // Sat 04:00 -> still Friday's window
  TEST_ASSERT_NOT_NULL(activeWindow(g_cfg, t));
  t.hour = 5;                                             // end is exclusive
  TEST_ASSERT_NULL(activeWindow(g_cfg, t));
  t.wday = 5; t.hour = 22;                                // Fri 22:00
  TEST_ASSERT_NOT_NULL(activeWindow(g_cfg, t));
  DecisionInput in;
  in.cfg = &g_cfg;
  in.logic = &g_ref;
  in.clock_valid = true;
  in.now = t;
  in.ovr.mode = Mode::Schedule;
  Decision d = decide(in);
  TEST_ASSERT_TRUE(d.relay);
  TEST_ASSERT_EQUAL_STRING("Inside schedule window Fri 22:00–05:00", d.reason);
  in.now.hour = 21;
  TEST_ASSERT_EQUAL_STRING("Outside schedule windows (schedule only)", decide(in).reason);
  in.ovr.mode = Mode::SchedulePrice;   // falls through to price logic (unknown price -> default)
  TEST_ASSERT_EQUAL((int)DecisionSrc::Default, (int)decide(in).src);
}

static void test_validation_errors() {
  const char* j = R"({"state_select":"nope","states":{"Заряд":{"entry_conditions":[{"type":"soc","operator":"=>","value":120}],
    "charging_logic":[{"soc":10,"price_on":"max","price_off":1,"known_minutes":60}]}}})";
  LogicConfig l;
  std::vector<std::string> e;
  TEST_ASSERT_FALSE(parse(j, strlen(j), l, &e));
  TEST_ASSERT_EQUAL(5, e.size());
  TEST_ASSERT_EQUAL_STRING("Заряд: block 1 price_on must be a number or \"min\"", e[2].c_str());
  TEST_ASSERT_EQUAL_STRING("state_select “nope” is not a state", e[4].c_str());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reference_parse_and_roundtrip);
  RUN_TEST(test_stickiness);
  RUN_TEST(test_operators_literal_and_overlap);
  RUN_TEST(test_state_select_and_blocks);
  RUN_TEST(test_transitions);
  RUN_TEST(test_precedence_and_default);
  RUN_TEST(test_schedule_overnight);
  RUN_TEST(test_validation_errors);
  return UNITY_END();
}
