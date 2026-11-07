// On-device ComEd fetch loop (PLAN §6.1). Owner: price.
#include "price/price_fetcher.h"

#include <stdio.h>
#include <string.h>

#include "net/https_client.h"
#include "price/comed_parse.h"

namespace prices {

static const char* kFiveUrl = "https://hourlypricing.comed.com/api?type=5minutefeed";
static const char* kServletUrl = "https://hourlypricing.comed.com/rrtp/ServletFeed?type=%s&date=%lu";
static const int64_t kRetryS = 60;        // retry delay after a failed fetch
static const int64_t kHourlyS = 3600;

static PriceSnapshot s_snap;
static FivePoint s_scratch[kFiveMax];     // parse target; swapped in only on success
static char s_err[48] = "";

static int64_t s_five_due = 0;            // epoch; 0 = due now
static int64_t s_today_due = 0, s_tomorrow_due = 0, s_actual_due = 0;
static uint8_t s_tomorrow_count = 0;

void begin() { s_five_due = s_today_due = s_tomorrow_due = s_actual_due = 0; }
void forceFiveMin() { s_five_due = 0; }
PriceSnapshot& snapshot() { return s_snap; }
const char* lastError() { return s_err; }

static void setErr(const https::Result& r) {
  if (r.ok()) { s_err[0] = 0; return; }
  if (*r.error) snprintf(s_err, sizeof(s_err), "%s", r.error);
  else snprintf(s_err, sizeof(s_err), "http %d", r.status);
}

static bool fetchFive(const ChicagoTime& now) {
  price::FiveMinParser p(s_scratch, kFiveMax);
  https::Result r = https::get(kFiveUrl, [&](const uint8_t* d, size_t n) {
    p.feed((const char*)d, n);
    return true;
  });
  if (r.ok() && (!p.ok() || p.count() == 0)) r.error = "bad 5min data";
  setErr(r);
  if (!r.ok()) { s_five_due = now.epoch + kRetryS; return false; }
  memcpy(s_snap.five, s_scratch, p.count() * sizeof(FivePoint));
  s_snap.five_count = (uint16_t)p.count();
  s_snap.five_fetched_at = now.epoch;
  // next :01, :06, :11, ... (Chicago offsets are whole multiples of 5 min, so UTC alignment is equal)
  s_five_due = ((now.epoch - 60) / 300 + 1) * 300 + 60;
  return true;
}

// Fetch one ServletFeed day into out[24]; returns count (0 on failure, error recorded).
static uint8_t fetchServlet(const char* type, uint32_t date, float out[24]) {
  char url[128];
  snprintf(url, sizeof(url), kServletUrl, type, (unsigned long)date);
  price::ServletParser p(out);
  https::Result r = https::get(url, [&](const uint8_t* d, size_t n) {
    p.feed((const char*)d, n);
    return true;
  });
  if (r.ok() && p.count() == 0) r.error = "bad servlet data";
  setErr(r);
  return r.ok() ? p.count() : 0;
}

bool tick(const ChicagoTime& now) {
  if (now.epoch < kMinValidEpoch) return false;
  const int64_t t = now.epoch;
  const uint32_t today = now.yyyymmdd(), tomorrow = addDays(today, 1);
  float tmp[24];

  if (t >= s_five_due) return fetchFive(now);

  if (s_snap.da_today_date != today || t >= s_today_due) {
    if (s_snap.da_today_date != today && s_snap.da_tomorrow_date == today) {
      // tomorrow's feed became today's: use it right away, refresh below
      memcpy(s_snap.da_today, s_snap.da_tomorrow, sizeof(tmp));
      s_snap.da_today_date = today;
      s_today_due = t + kHourlyS;
      return true;
    }
    uint8_t n = fetchServlet("daynexttoday", today, tmp);
    if (!n) { s_today_due = t + kRetryS; return false; }
    memcpy(s_snap.da_today, tmp, sizeof(tmp));
    s_snap.da_today_date = today;
    s_snap.da_fetched_at = t;
    s_today_due = t + kHourlyS;
    return true;
  }

  // Tomorrow: only at/after 17:00 Chicago; cached per date (re-polled hourly while incomplete).
  if (now.hour >= 17 && (s_snap.da_tomorrow_date != tomorrow || (s_tomorrow_count < 24 && t >= s_tomorrow_due))) {
    uint8_t n = fetchServlet("daynexttoday", tomorrow, tmp);
    if (!n) { s_tomorrow_due = t + kRetryS; return false; }
    memcpy(s_snap.da_tomorrow, tmp, sizeof(tmp));
    s_snap.da_tomorrow_date = tomorrow;
    s_tomorrow_count = n;
    s_snap.da_fetched_at = t;
    s_tomorrow_due = t + kHourlyS;
    return true;
  }

  if (s_snap.actual_date != today || t >= s_actual_due) {
    uint8_t n = fetchServlet("day", today, tmp);
    if (!n) { s_actual_due = t + kRetryS; return false; }
    memcpy(s_snap.actual_today, tmp, sizeof(tmp));
    s_snap.actual_date = today;
    s_snap.actual_fetched_at = t;
    s_actual_due = t + kHourlyS;
    return false;  // display only: decision inputs unchanged
  }
  return false;
}

}  // namespace prices
