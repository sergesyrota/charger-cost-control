// STUB — owner: web_api.
#include "web/server.h"

#include "web/api.h"
#include "web/ui_gz.h"

namespace web {
static AsyncWebServer s_server(80);
static bool s_up = false;
void begin() {
  s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    AsyncWebServerResponse* res = r->beginResponse(200, "text/html", ui_gz, ui_gz_len);
    res->addHeader("Content-Encoding", "gzip");
    r->send(res);
  });
  api::registerRoutes(s_server);
  s_server.begin();
  s_up = true;
}
void loop() {}
AsyncWebServer& server() { return s_server; }
bool serving() { return s_up; }
}  // namespace web
