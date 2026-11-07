// Owner: car. Sparse, high-value only (ELM echo on/off parsing, PID formulas, classification).
#include <unity.h>

#include "car/car_status.h"
#include "car/obd_pids.h"

void setUp() {}
void tearDown() {}

static void test_placeholder() {
  TEST_ASSERT_EQUAL_STRING("228334", obd::kSoc.cmd);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_placeholder);
  return UNITY_END();
}
