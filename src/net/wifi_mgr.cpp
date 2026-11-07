// STUB — owner: net.
#include "net/wifi_mgr.h"

namespace wifi {
void begin(uint16_t) {}
void loop() {}
void setTimeout(uint16_t) {}
bool hasCredentials() { return false; }
bool passwordSet() { return false; }
void setCredentials(const char*, const char*) {}
bool staConnected() { return false; }
bool apActive() { return false; }
String ssid() { return String(); }
int rssi() { return 0; }
String ip() { return String(); }
String apSsid() { return String(); }
String apIp() { return String(); }
uint8_t apClients() { return 0; }
bool startScan() { return false; }
bool scanRunning() { return false; }
int scanResults(Network*, int) { return -1; }
}  // namespace wifi
