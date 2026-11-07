// HTTP :80 — gzipped UI, captive portal (hotspot), JSON API mount. Owner: web_api.
#include "web/server.h"

#include <WiFi.h>

#include "net/wifi_mgr.h"
#include "web/api.h"
#include "web/ui_gz.h"

namespace web {
namespace {
AsyncWebServer s_server(80);
bool s_up = false;
char s_etag[24] = "";

void redirectToPortal(AsyncWebServerRequest* r) {
  String ip = wifi::apIp();
  if (ip.length() == 0) ip = "192.168.4.1";
  r->redirect("http://" + ip + "/");
}

void sendUi(AsyncWebServerRequest* r) {
  if (r->hasHeader("If-None-Match") && r->header("If-None-Match") == s_etag) {
    AsyncWebServerResponse* nm = r->beginResponse(304);
    nm->addHeader("ETag", s_etag);
    nm->addHeader("Cache-Control", "no-cache");
    r->send(nm);
    return;
  }
  AsyncWebServerResponse* res = r->beginResponse(200, "text/html", ui_gz, ui_gz_len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");  // always revalidate; ETag makes it a cheap 304
  res->addHeader("ETag", s_etag);
  r->send(res);
}
}  // namespace

void begin() {
  uint32_t h = 2166136261u;  // FNV-1a over the embedded UI -> ETag
  for (size_t i = 0; i < ui_gz_len; i++) h = (h ^ ui_gz[i]) * 16777619u;
  snprintf(s_etag, sizeof(s_etag), "\"%08x-%u\"", (unsigned)h, (unsigned)ui_gz_len);

  s_server.on("/", HTTP_GET, sendUi);
  s_server.on("/index.html", HTTP_GET, sendUi);
  api::registerRoutes(s_server);
  s_server.onNotFound([](AsyncWebServerRequest* r) {
    if (r->url().startsWith("/api/")) {
      r->send(404, "application/json", "{\"error\":\"not found\"}");
    } else if (wifi::apActive()) {
      redirectToPortal(r);  // captive portal probes (generate_204, hotspot-detect.html, ncsi.txt, ...)
    } else {
      r->send(404, "text/plain", "Not found");
    }
  });
  s_server.begin();
  s_up = true;
}

// Captive DNS is owned by wifi_mgr (starts/stops with the hotspot); nothing to do here.
void loop() {}

AsyncWebServer& server() { return s_server; }
bool serving() { return s_up; }
}  // namespace web
