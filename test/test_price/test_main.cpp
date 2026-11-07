// Owner: price. Sparse, high-value only (port key prices.test.ts cases + fixture parse).
#include <unity.h>

#include "price/price_math.h"
#include "util/chicago_time.h"

void setUp() {}
void tearDown() {}

static void test_chicago_time() {
  ChicagoTime t = chicagoFromEpoch(1794089100);  // 2026-11-07 fixture time (CST)
  TEST_ASSERT_EQUAL_INT32(-21600, t.utc_offset_s);
  TEST_ASSERT_EQUAL_UINT32(20261107, t.yyyymmdd());
  TEST_ASSERT_EQUAL_UINT32(20261108, addDays(t.yyyymmdd(), 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_chicago_time);
  return UNITY_END();
}
