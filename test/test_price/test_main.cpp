// Owner: price. Sparse, high-value only: fixture parsing + prices.ts semantics.
// Expected values were computed by running the reference TS logic (price-worker) with node
// against the same fixtures.
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unity.h>

#include "price/comed_parse.h"
#include "price/price_math.h"
#include "util/chicago_time.h"

void setUp() {}
void tearDown() {}

static std::string slurp(const char* name) {
  std::string path = std::string("test/fixtures/") + name;
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) f = fopen((std::string("../../") + path).c_str(), "rb");
  TEST_ASSERT_NOT_NULL_MESSAGE(f, name);
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static PriceSnapshot snap;  // large; keep off the stack
static float today[24], tomorrow[24];

static void loadFixtures() {
  std::string j = slurp("5min.json");
  snap.five_count = (uint16_t)price::parseFiveMin(j.data(), j.size(), snap.five, kFiveMax);
  std::string a = slurp("daynexttoday_20261107.txt"), b = slurp("daynexttoday_20261108.txt");
  price::parseServlet(a.data(), a.size(), today);
  price::parseServlet(b.data(), b.size(), tomorrow);
}

static void test_parse_fixtures() {
  loadFixtures();
  TEST_ASSERT_EQUAL_UINT16(287, snap.five_count);
  TEST_ASSERT_EQUAL_UINT32(1794089100, snap.five[0].epoch);  // newest first
  TEST_ASSERT_EQUAL_FLOAT(2.1f, snap.five[0].price);
  TEST_ASSERT_EQUAL_FLOAT(3.5f, today[0]);  // hour-ending index 0
  TEST_ASSERT_EQUAL_FLOAT(2.4f, today[23]);
  TEST_ASSERT_EQUAL_FLOAT(6.0f, today[17]);

  // 1-byte chunks must give identical results
  std::string j = slurp("5min.json");
  static FivePoint pts[kFiveMax];
  price::FiveMinParser p(pts, kFiveMax);
  for (char c : j) p.feed(&c, 1);
  TEST_ASSERT_TRUE(p.ok());
  TEST_ASSERT_EQUAL_UINT32(287, p.count());
  TEST_ASSERT_EQUAL_FLOAT(snap.five[286].price, pts[286].price);
  std::string a = slurp("daynexttoday_20261107.txt");
  float h[24];
  price::ServletParser sp(h);
  for (char c : a) sp.feed(&c, 1);
  TEST_ASSERT_EQUAL_UINT8(24, sp.count());
  TEST_ASSERT_EQUAL_FLOAT(today[5], h[5]);

  // actuals feed is partial (hours 0..16): missing hours stay NAN
  std::string d = slurp("day_20261107.txt");
  float act[24];
  TEST_ASSERT_EQUAL_UINT8(17, price::parseServlet(d.data(), d.size(), act));
  TEST_ASSERT_TRUE(isnan(act[17]));
}

static void test_current_supply_and_lowest() {
  loadFixtures();
  // {now epoch, expected current, expected lowest7 (hour<17 uses today[0..7])}
  struct C { int64_t t; float cur; float low; } cases[] = {
      {1794089160, 7.3f, 3.1f},  // 16:06 CST: only 10 known min -> mean of 6 most recent
      {1794069360, 1.8f, 3.1f},  // 10:36: >= 30 known min -> hour average
      {1794060600, 2.1f, 3.1f},  // 08:10
  };
  for (auto& c : cases) {
    ChicagoTime now = chicagoFromEpoch(c.t);
    bool est = true;
    float cur = price::currentSupply(snap.five, snap.five_count, today, now, &est);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, c.cur, cur);
    TEST_ASSERT_FALSE(est);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, c.low, price::lowestBy7amSupply(today, tomorrow, now.hour));
  }
  // >= 17:00 -> remaining today (idx 19..23) + tomorrow[0..7]; min is tomorrow[3] = 1.5
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, price::lowestBy7amSupply(today, tomorrow, 18));
  // no tomorrow yet: only the rest of today
  float none[24];
  for (float& f : none) f = NAN;
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.4f, price::lowestBy7amSupply(today, none, 18));
  TEST_ASSERT_TRUE(isnan(price::lowestBy7amSupply(none, none, 3)));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.6f, price::minPrice(3.1f, 0.5f));
}

static void test_derive_staleness_and_dates() {
  loadFixtures();
  for (int i = 0; i < 24; i++) { snap.da_today[i] = today[i]; snap.da_tomorrow[i] = tomorrow[i]; }
  snap.da_today_date = 20261107;
  snap.da_tomorrow_date = 0;
  snap.five_fetched_at = 1794089160;
  ChicagoTime now = chicagoFromEpoch(1794089160 + 60);  // 16:07 CST
  price::derive(snap, now, 20, 0.5f);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.3f, snap.current);
  TEST_ASSERT_FALSE(snap.estimated);
  TEST_ASSERT_EQUAL_UINT16(10, snap.known_minutes);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 3.6f, snap.min_price);

  // 5-min feed stale (> 20 min) -> falls back to day-ahead[(16+1)%24] = 6.0, estimated
  now = chicagoFromEpoch(1794089160 + 21 * 60);
  price::derive(snap, now, 20, 0.5f);
  TEST_ASSERT_TRUE(snap.estimated);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 6.0f, snap.current);

  // day-ahead for the wrong date is ignored -> everything unknown (=> logic default)
  snap.da_today_date = 20261106;
  price::derive(snap, now, 20, 0.5f);
  TEST_ASSERT_TRUE(isnan(snap.current));
  TEST_ASSERT_TRUE(isnan(snap.min_price));
}

static void test_round1() {
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.2f, price::round1(2.15f + 0.0001f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, -1.2f, price::round1(-1.15f - 0.001f));
  TEST_ASSERT_TRUE(isnan(price::round1(NAN)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_parse_fixtures);
  RUN_TEST(test_current_supply_and_lowest);
  RUN_TEST(test_derive_staleness_and_dates);
  RUN_TEST(test_round1);
  return UNITY_END();
}
