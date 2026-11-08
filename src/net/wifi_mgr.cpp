// Wi-Fi connectivity state machine (PLAN §11 "Connectivity"). Owner: net.
//
// Credentials are kept in our own NVS namespace "wifi" (keys "ssid", "pass"); the driver's own
// persistence is disabled so there is exactly one copy. The password never leaves this file.
//
// Phases (all timing in loop(), millis based):
//   no creds            -> AP (+idle STA for scans), captive DNS answers every name with the AP IP
//   creds, STA down     -> STA only, retry every 15 s; after timeout_s continuous down -> AP+STA,
//                          retry every 60 s
//   creds, STA up       -> drop AP once no client is associated (or 10 min after STA came up)
// A scan pauses STA retries (the radio can't scan while connecting).
#include "net/wifi_mgr.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <utility>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "store/event_log.h"

namespace wifi {

namespace {

constexpr uint32_t kRetryStaMs = 15000;
constexpr uint32_t kRetryApMs = 60000;
constexpr uint32_t kApGraceMs = 10UL * 60 * 1000;
constexpr uint32_t kScanTimeoutMs = 20000;
constexpr uint32_t kScanKeepMs = 60000;
constexpr int kMaxNets = 16;

SemaphoreHandle_t s_mx = nullptr;  // guards s_ssid, scan cache
struct Lock {
  Lock() {
    if (!s_mx) s_mx = xSemaphoreCreateMutex();  // begin() creates it first; this is a safety net
    xSemaphoreTake(s_mx, portMAX_DELAY);
  }
  ~Lock() { xSemaphoreGive(s_mx); }
};

char s_ssid[33] = "";
char s_pass[65] = "";  // write-only: never returned, never logged
char s_ap_ssid[24] = "";
volatile bool s_has_creds = false;
volatile bool s_ap = false;
volatile bool s_sta_up = false;   // set by the event handler (GOT_IP / DISCONNECTED)
volatile uint32_t s_ip = 0;

uint32_t s_timeout_ms = 300000;
bool s_was_up = false;
uint32_t s_down_since = 0, s_up_since = 0, s_last_try = 0;
bool s_mdns = false;
uint32_t s_mdns_try = 0;

DNSServer s_dns;

// scan
volatile bool s_scan_req = false;
volatile bool s_scanning = false;
uint32_t s_scan_started = 0;
Network s_nets[kMaxNets];
int s_net_count = -1;
uint32_t s_scan_at = 0;

void onEvent(arduino_event_id_t ev, arduino_event_info_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      s_ip = info.got_ip.ip_info.ip.addr;
      s_sta_up = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
    case ARDUINO_EVENT_WIFI_STA_STOP:
      s_sta_up = false;
      s_ip = 0;
      break;
    default:
      break;
  }
}

void loadCreds() {
  Preferences p;
  if (p.begin("wifi", true)) {
    p.getString("ssid", s_ssid, sizeof(s_ssid));
    p.getString("pass", s_pass, sizeof(s_pass));
    p.end();
  }
  s_has_creds = s_ssid[0] != 0;
}

void saveCreds() {
  Preferences p;
  if (!p.begin("wifi", false)) return;
  if (s_ssid[0]) {
    p.putString("ssid", s_ssid);
    p.putString("pass", s_pass);
  } else {
    p.clear();
  }
  p.end();
}

void tryConnect() {
  s_last_try = millis();
  if (!s_has_creds) return;
  WiFi.begin(s_ssid, s_pass[0] ? s_pass : nullptr);
}

void startAp() {
  if (s_ap) return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(s_ap_ssid);  // open (trusted LAN, owner decision)
  s_dns.setErrorReplyCode(DNSReplyCode::NoError);
  s_dns.start(53, "*", WiFi.softAPIP());
  s_ap = true;
  evlog::add(evlog::Kind::Sys, "Точка доступа %s включена, %s", s_ap_ssid, WiFi.softAPIP().toString().c_str());
}

void stopAp() {
  if (!s_ap) return;
  s_dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  s_ap = false;
  evlog::add(evlog::Kind::Sys, "Точка доступа выключена");
}

void startMdns(uint32_t now) {
  if (s_mdns || (s_mdns_try && now - s_mdns_try < 30000)) return;
  s_mdns_try = now;
  if (MDNS.begin("charger")) {
    MDNS.addService("http", "tcp", 80);
    s_mdns = true;
  }
}

void collectScan(int n) {
  Network tmp[kMaxNets];
  int cnt = 0;
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (!name.length()) continue;
    int8_t rssi = (int8_t)WiFi.RSSI(i);
    int found = -1;
    for (int j = 0; j < cnt; j++)
      if (tmp[j].ssid == name) { found = j; break; }
    if (found >= 0) {
      if (rssi > tmp[found].rssi) {
        tmp[found].rssi = rssi;
        tmp[found].channel = (uint8_t)WiFi.channel(i);
        tmp[found].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      }
      continue;
    }
    if (cnt < kMaxNets) {
      tmp[cnt] = Network{name, rssi, (uint8_t)WiFi.channel(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
      cnt++;
    } else {  // full: replace the weakest if this one is stronger
      int w = 0;
      for (int j = 1; j < cnt; j++)
        if (tmp[j].rssi < tmp[w].rssi) w = j;
      if (rssi > tmp[w].rssi)
        tmp[w] = Network{name, rssi, (uint8_t)WiFi.channel(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN};
    }
  }
  for (int i = 1; i < cnt; i++)  // insertion sort, strongest first
    for (int j = i; j > 0 && tmp[j].rssi > tmp[j - 1].rssi; j--) std::swap(tmp[j], tmp[j - 1]);
  Lock l;
  for (int i = 0; i < cnt; i++) s_nets[i] = tmp[i];
  s_net_count = cnt;
  s_scan_at = millis();
}

void scanLoop(uint32_t now) {
  if (s_scanning) {
    int16_t r = WiFi.scanComplete();
    if (r >= 0) {
      collectScan(r);
      WiFi.scanDelete();
    } else if (r == WIFI_SCAN_RUNNING && now - s_scan_started < kScanTimeoutMs) {
      return;
    } else {  // failed / timed out: publish an empty result so the UI doesn't spin forever
      WiFi.scanDelete();
      Lock l;
      s_net_count = 0;
      s_scan_at = now;
    }
    s_scanning = false;
    s_last_try = now - (s_ap ? kRetryApMs : kRetryStaMs) + 2000;  // resume STA attempts shortly
    return;
  }
  if (!s_scan_req) return;
  s_scan_req = false;
  if (!s_sta_up && s_has_creds) WiFi.disconnect(false, false);  // abort a pending connect attempt
  if (WiFi.scanNetworks(true, false) == WIFI_SCAN_FAILED) {
    Lock l;
    s_net_count = 0;
    s_scan_at = now;
    return;
  }
  s_scanning = true;
  s_scan_started = now;
}

}  // namespace

void begin(uint16_t connect_timeout_s) {
  if (!s_mx) s_mx = xSemaphoreCreateMutex();
  setTimeout(connect_timeout_s);
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ChargerControl-%02X%02X", mac[4], mac[5]);
  loadCreds();

  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);  // retries are paced by loop()
  WiFi.onEvent(onEvent);
  WiFi.setHostname("charger");
  uint32_t now = millis();
  s_down_since = now;
  if (s_has_creds) {
    WiFi.mode(WIFI_STA);
    tryConnect();
  } else {
    startAp();
  }
}

void loop() {
  uint32_t now = millis();
  startMdns(now);
  scanLoop(now);

  if (!s_has_creds) {
    startAp();
    return;
  }

  bool up = s_sta_up;
  if (up) {
    if (!s_was_up) {
      s_was_up = true;
      s_up_since = now;
      Serial.printf("Wi-Fi connected: %s IP %s\n", s_ssid, ip().c_str());
      evlog::add(evlog::Kind::Sys, "Wi-Fi: подключено к %s, IP %s, %d дБм", s_ssid, ip().c_str(), rssi());
    }
    if (s_ap && (WiFi.softAPgetStationNum() == 0 || now - s_up_since >= kApGraceMs)) stopAp();
    return;
  }

  if (s_was_up) {
    s_was_up = false;
    s_down_since = now;
    evlog::add(evlog::Kind::Sys, "Wi-Fi: соединение потеряно");
  }
  if (!s_ap && now - s_down_since >= s_timeout_ms) startAp();
  if (!s_scanning && !s_scan_req && now - s_last_try >= (s_ap ? kRetryApMs : kRetryStaMs)) tryConnect();
}

void setTimeout(uint16_t connect_timeout_s) {
  if (connect_timeout_s < 30) connect_timeout_s = 30;
  s_timeout_ms = (uint32_t)connect_timeout_s * 1000;
}

bool hasCredentials() { return s_has_creds; }
bool passwordSet() { return s_pass[0] != 0; }

void setCredentials(const char* ssid, const char* pass) {
  const char* ns = ssid ? ssid : "";
  bool same = strcmp(ns, s_ssid) == 0;
  {
    Lock l;
    strlcpy(s_ssid, ns, sizeof(s_ssid));
  }
  if (pass && *pass) strlcpy(s_pass, pass, sizeof(s_pass));
  else if (!same) s_pass[0] = 0;  // new SSID without password => open network
  s_has_creds = s_ssid[0] != 0;
  saveCreds();

  WiFi.disconnect(false, false);
  s_sta_up = false;
  s_ip = 0;
  s_was_up = false;
  uint32_t now = millis();
  s_down_since = now;  // restart the 5-min STA-only cycle
  if (!s_has_creds) {
    evlog::add(evlog::Kind::Sys, "Wi-Fi: данные сети удалены");
    startAp();
    return;
  }
  evlog::add(evlog::Kind::Sys, "Wi-Fi: сохранена сеть %s", s_ssid);
  // If the hotspot is up (user is configuring through it) keep it until STA connects and the
  // client leaves; otherwise STA only.
  if (!s_ap) WiFi.mode(WIFI_STA);
  tryConnect();
}

bool staConnected() { return s_sta_up; }
bool apActive() { return s_ap; }

String ssid() {
  char buf[33];
  {
    Lock l;
    strlcpy(buf, s_ssid, sizeof(buf));
  }
  return String(buf);
}

int rssi() { return s_sta_up ? WiFi.RSSI() : 0; }

String ip() {
  uint32_t a = s_ip;
  if (!s_sta_up || !a) return String();
  return IPAddress(a).toString();
}

String apSsid() { return String(s_ap_ssid); }
String apIp() { return s_ap ? WiFi.softAPIP().toString() : String(); }
uint8_t apClients() { return s_ap ? (uint8_t)WiFi.softAPgetStationNum() : 0; }

bool startScan() {
  if (s_scan_req || s_scanning) return false;
  s_scan_req = true;
  return true;
}

bool scanRunning() { return s_scan_req || s_scanning; }

int scanResults(Network* out, int max) {
  Lock l;
  if (s_net_count < 0 || millis() - s_scan_at > kScanKeepMs) return -1;
  int n = s_net_count < max ? s_net_count : max;
  for (int i = 0; i < n; i++) out[i] = s_nets[i];
  return n;
}

}  // namespace wifi
