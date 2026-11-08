// Task-watchdog feed for long blocking work on the loop task (BLE waits, TLS streams).
// Safe from any task: only resets when the calling task is subscribed to the TWDT.
#pragma once
#include <esp_task_wdt.h>
inline void wdtFeed() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
}
