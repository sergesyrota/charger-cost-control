// STUB — owner: web_api.
#include "web/api.h"

namespace api {
void registerRoutes(AsyncWebServer& s) {
  s.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    r->send(501, "application/json", "{\"error\":\"not implemented\"}");
  });
}
}  // namespace api
