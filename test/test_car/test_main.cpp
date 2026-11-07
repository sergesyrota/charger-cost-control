// Owner: car. Sparse, high-value only (ELM echo on/off parsing, PID formulas, classification).
#include <string.h>
#include <unity.h>

#include "car/car_status.h"
#include "car/obd_pids.h"

using namespace obd;

void setUp() {}
void tearDown() {}

static ElmResponse P(const char* raw, const char* cmd) { return parseResponse(raw, strlen(raw), cmd); }

static void test_elm_parse() {
  // echo on, headers off (dongle default; what the Pi saw)
  ElmResponse r = P("228334\r62 83 34 4F \r\r>", "228334");
  TEST_ASSERT_EQUAL(ElmStatus::Ok, r.status);
  TEST_ASSERT_EQUAL(3, r.len);
  TEST_ASSERT_EQUAL_HEX8(0x4F, r.bytes[2]);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 30.98f, socPct(r));
  // echo on, headers on
  r = P("22432D\r7EC 05 62 43 2D 02 97 \r\r>", "22432D");
  TEST_ASSERT_EQUAL(ElmStatus::Ok, r.status);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 344.76f, packVolts(r));
  // echo off, headers on
  r = P("7EC 04 62 83 34 78 \r\r>", "228334");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 47.06f, socPct(r));
  // echo off, headers off (extension over the Python)
  r = P("62 83 34 78\r\r>", "228334");
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 47.06f, socPct(r));
  // errors
  TEST_ASSERT_EQUAL(ElmStatus::CommandError, P("228334\rCAN ERROR\r\r>", "228334").status);
  TEST_ASSERT_EQUAL(ElmStatus::NoData, P("228334\rNO DATA\r\r>", "228334").status);
  TEST_ASSERT_EQUAL(ElmStatus::Malformed, P("228334\r7F 22 31\r\r>", "228334").status);
  TEST_ASSERT_TRUE(isnan(socPct(P("228334\rNO DATA\r>", "228334"))));
}

static void test_formulas_and_status() {
  // current: signed A. 0xFF,0x92 => (-1*256 + 146)/20 = -5.5 A ; 0x01,0x00 => 12.8 A
  TEST_ASSERT_FLOAT_WITHIN(0.001f, -5.5f, packAmps(P("2240D4\r62 40 D4 FF 92\r>", "2240D4")));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.8f, packAmps(P("2240D4\r62 40 D4 01 00\r>", "2240D4")));
  char buf[8];
  TEST_ASSERT_EQUAL_STRING("224181", cellPid(1, buf).cmd);
  TEST_ASSERT_EQUAL_STRING("22419F", cellPid(31, buf).cmd);
  TEST_ASSERT_EQUAL_STRING("224240", cellPid(96, buf).cmd);
  // classification: sign unknown -> |W|
  TEST_ASSERT_EQUAL(CarStatus::Charging, classify(true, ElmStatus::Ok, -3000.f, 500, 0));
  TEST_ASSERT_EQUAL(CarStatus::Online, classify(true, ElmStatus::Ok, -3000.f, 500, 1));
  TEST_ASSERT_EQUAL(CarStatus::Online, classify(true, ElmStatus::NoData, NAN, 500, 0));
  TEST_ASSERT_EQUAL(CarStatus::Asleep, classify(true, ElmStatus::CommandError, NAN, 500, 0));
  TEST_ASSERT_EQUAL(CarStatus::NotDetected, classify(false, ElmStatus::Ok, 0, 500, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_elm_parse);
  RUN_TEST(test_formulas_and_status);
  return UNITY_END();
}
