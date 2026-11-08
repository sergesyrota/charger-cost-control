// JSON API handlers — the contract is docs/API.md. Owner: web_api.
// Handlers run on the AsyncTCP task: never block, never do TLS/BLE. Read via app:: getters and
// thread-safe module getters (wifi::, ota::progress, stats::, evlog::); mutate only via app::.
#pragma once
#include <ESPAsyncWebServer.h>

namespace api {

void registerRoutes(AsyncWebServer& s);

}  // namespace api
