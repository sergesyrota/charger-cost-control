// Wi-Fi connectivity per PLAN §11 "Connectivity". Owner: net.
//  - No saved credentials -> open hotspot "ChargerControl-XXXX" (192.168.4.1) immediately.
//  - Credentials -> STA only for cfg.wifi_timeout_s (default 300 s); still down -> AP+STA, retry STA
//    every 60 s; once STA is up, drop AP (wait while a client is associated, max 10 min).
//  - Never run the hotspot alongside a working STA (except that grace).
//  - Credentials live in the Wi-Fi driver's NVS; the password is NEVER readable through this API.
//  - mDNS "charger.local" best effort.
// All getters are thread-safe (called from web handlers).
#pragma once
#include <Arduino.h>

namespace wifi {

void begin(uint16_t connect_timeout_s);
void loop();                               // call every loop() iteration; non-blocking
void setTimeout(uint16_t connect_timeout_s);

bool hasCredentials();
bool passwordSet();                        // a non-empty password is stored
// Save + reconnect (from the loop task only — app queues it). pass == nullptr or "" keeps the
// current password if ssid is unchanged; for a new ssid it means "open network".
void setCredentials(const char* ssid, const char* pass);
// Erase the saved network (NVS + RAM) without touching the radio (loop task only). Meant to be
// followed by a reboot, which then comes up as the hotspot (no credentials).
void forget();

bool staConnected();
bool apActive();
String ssid();                             // configured SSID ("" if none)
int rssi();                                // dBm, 0 if not connected
String ip();                               // STA IP or ""
String apSsid();                           // e.g. "ChargerControl-2118"
String apIp();                             // "192.168.4.1" when AP active, else ""
uint8_t apClients();

// Async scan. startScan() returns false if one is already running.
struct Network { String ssid; int8_t rssi; uint8_t channel; bool secure; };
bool startScan();
bool scanRunning();
// Copies up to max results (deduped by SSID, strongest first); returns count, -1 if none available.
int scanResults(Network* out, int max);

}  // namespace wifi
