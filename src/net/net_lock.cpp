// Recursive network lock. Owner: net.
#include "net/net_lock.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace netlock {

static SemaphoreHandle_t s_mx = nullptr;
static const char* volatile s_holder = "";
static int s_depth = 0;  // only touched by the task holding the mutex

void init() {
  if (!s_mx) s_mx = xSemaphoreCreateRecursiveMutex();
}

bool take(const char* who, uint32_t timeout_ms) {
  if (!s_mx) init();
  if (xSemaphoreTakeRecursive(s_mx, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) return false;
  if (s_depth++ == 0) s_holder = who ? who : "?";
  return true;
}

void give() {
  if (!s_mx) return;
  if (s_depth > 0 && --s_depth == 0) s_holder = "";
  xSemaphoreGiveRecursive(s_mx);
}

const char* holder() { return s_holder; }

}  // namespace netlock
