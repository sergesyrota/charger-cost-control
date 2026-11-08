// SNTP clock. Owner: net.
#include "net/clock.h"

#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_timer.h>
#include <time.h>

namespace sysclock {

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_synced_at = 0;
static bool s_started = false;

static void onSync(struct timeval* tv) {
  int64_t t = tv ? (int64_t)tv->tv_sec : (int64_t)time(nullptr);
  portENTER_CRITICAL(&s_mux);
  s_synced_at = t;
  portEXIT_CRITICAL(&s_mux);
}

void begin() {
  if (s_started) return;
  s_started = true;
  sntp_set_time_sync_notification_cb(onSync);
  configTzTime("CST6CDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.google.com");
}

static int64_t raw() { return (int64_t)time(nullptr); }

bool valid() { return raw() >= kMinValidEpoch; }

int64_t now() {
  int64_t t = raw();
  return t >= kMinValidEpoch ? t : 0;
}

bool chicago(ChicagoTime& out) {
  int64_t t = now();
  if (!t) return false;
  out = chicagoFromEpoch(t);  // pure helper; same rules as the POSIX TZ above
  return true;
}

uint32_t uptimeS() { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

int64_t syncedAt() {
  portENTER_CRITICAL(&s_mux);
  int64_t t = s_synced_at;
  portEXIT_CRITICAL(&s_mux);
  return t;
}

}  // namespace sysclock
