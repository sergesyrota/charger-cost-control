// Owner: store. Sparse: config secrets contract + record codec round trip.
#include <unity.h>

#include <string.h>

#include "store/config_codec.h"
#include "store/records.h"

void setUp() {}
void tearDown() {}

static void test_secrets_write_only() {
  Config c;
  std::string err;
  JsonDocument in;
  deserializeJson(in, "{\"pushover\":{\"enabled\":true,\"token\":\"tok123\",\"user\":\"usr456\"},\"soc_target\":70}");
  TEST_ASSERT_TRUE(cfgjson::mergeApi(in.as<JsonObjectConst>(), c, err));
  TEST_ASSERT_EQUAL_STRING("tok123", c.pushover_token);

  JsonDocument out;
  cfgjson::toApi(c, out.to<JsonObject>());
  std::string api;
  serializeJson(out, api);
  TEST_ASSERT_NULL(strstr(api.c_str(), "tok123"));
  TEST_ASSERT_NULL(strstr(api.c_str(), "usr456"));
  TEST_ASSERT_TRUE(out["pushover"]["token_set"].as<bool>());

  // empty keeps, clear erases, bad input leaves config untouched
  deserializeJson(in, "{\"pushover\":{\"token\":\"\"}}");
  TEST_ASSERT_TRUE(cfgjson::mergeApi(in.as<JsonObjectConst>(), c, err));
  TEST_ASSERT_EQUAL_STRING("tok123", c.pushover_token);
  deserializeJson(in, "{\"pushover\":{\"token_clear\":true}}");
  TEST_ASSERT_TRUE(cfgjson::mergeApi(in.as<JsonObjectConst>(), c, err));
  TEST_ASSERT_EQUAL_STRING("", c.pushover_token);
  deserializeJson(in, "{\"soc_target\":90,\"ota_url\":\"http://x\"}");
  TEST_ASSERT_FALSE(cfgjson::mergeApi(in.as<JsonObjectConst>(), c, err));
  TEST_ASSERT_EQUAL(70, c.soc_target);

  // storage round trip keeps secrets and the schedule
  c.window_count = 1;
  c.windows[0].days = 0x3E;
  c.windows[0].start_min = 60;
  c.windows[0].end_min = 300;
  c.windows[0].max_price = 3;
  strcpy(c.pushover_token, "tok123");
  std::string st = cfgjson::toStorage(c);
  Config d;
  TEST_ASSERT_TRUE(cfgjson::fromStorage(st.data(), st.size(), d));
  TEST_ASSERT_EQUAL_STRING("tok123", d.pushover_token);
  TEST_ASSERT_EQUAL(0x3E, d.windows[0].days);
  TEST_ASSERT_EQUAL(300, d.windows[0].end_min);
  TEST_ASSERT_EQUAL_FLOAT(3, d.windows[0].max_price);
}

static void test_records() {
  rec::Hourly h;
  h.hour_epoch = 1794060000;
  h.price10 = -15;
  h.kwh100 = 1234;
  h.relay_on_min = 42;
  uint8_t b[rec::kHourlySize];
  rec::encode(h, b);
  rec::Hourly r;
  TEST_ASSERT_TRUE(rec::decode(b, r));
  TEST_ASSERT_EQUAL(-15, r.price10);
  TEST_ASSERT_EQUAL(1234, r.kwh100);
  char row[96];
  TEST_ASSERT_TRUE(rec::toJsonRow(r, row, sizeof(row)) > 0);
  TEST_ASSERT_EQUAL_STRING("[1794060000,-1.5,null,42,12.34,null,null,0.00,0]", row);
  memset(b, 0xFF, sizeof(b));
  TEST_ASSERT_FALSE(rec::decode(b, r));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_secrets_write_only);
  RUN_TEST(test_records);
  return UNITY_END();
}
