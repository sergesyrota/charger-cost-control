// DISCOVERY SPIKE — not production code. Verifies toolchain, GPIO4, heap, BLE reach, OTA/rollback state.
#include <Arduino.h>
#include <WiFi.h>
#include <NetworkClientSecure.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <esp_flash.h>

static const int RELAY_PIN = 4;
static const char* DONGLE_MAC = "8c:de:52:de:b4:a0";
AsyncWebServer server(80);

static void heap(const char* tag) {
  Serial.printf("[heap] %-18s free=%u min=%u largest=%u\n", tag, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static const char* otaStateStr(esp_ota_img_states_t s) {
  switch (s) {
    case ESP_OTA_IMG_NEW: return "NEW";
    case ESP_OTA_IMG_PENDING_VERIFY: return "PENDING_VERIFY";
    case ESP_OTA_IMG_VALID: return "VALID";
    case ESP_OTA_IMG_INVALID: return "INVALID";
    case ESP_OTA_IMG_ABORTED: return "ABORTED";
    default: return "UNDEFINED";
  }
}

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  Serial.begin(115200);
  delay(3000);  // let USB CDC enumerate
  Serial.println("\n=== charger-cost-control discovery spike ===");
  Serial.printf("chip=%s rev=%d cores=%d cpu=%uMHz sdk=%s arduino=%d.%d.%d\n", ESP.getChipModel(),
                ESP.getChipRevision(), ESP.getChipCores(), ESP.getCpuFreqMHz(), ESP.getSdkVersion(),
                ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
  Serial.printf("flash=%u sketch=%u freeSketch=%u reset_reason=%d\n", ESP.getFlashChipSize(), ESP.getSketchSize(),
                ESP.getFreeSketchSpace(), (int)esp_reset_reason());
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  esp_err_t e = esp_ota_get_state_partition(run, &st);
  Serial.printf("running=%s state=%s (err=%d)\n", run->label, e == ESP_OK ? otaStateStr(st) : "n/a", e);
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
  Serial.println("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y (app sdkconfig)");
#else
  Serial.println("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE not set (app sdkconfig)");
#endif
  heap("boot");

  Serial.printf("LittleFS mount: %s\n", LittleFS.begin(true, "/littlefs", 10, "littlefs") ? "ok" : "FAIL");
  if (LittleFS.begin(false, "/littlefs", 10, "littlefs")) Serial.printf("LittleFS total=%u used=%u\n", LittleFS.totalBytes(), LittleFS.usedBytes());
  heap("after littlefs");

  Serial.println("GPIO4 blink x3 (2s on / 1s off)");
  for (int i = 0; i < 3; i++) {
    digitalWrite(RELAY_PIN, HIGH); delay(2000);
    digitalWrite(RELAY_PIN, LOW); delay(1000);
  }

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ChargerControl-spike");
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d; d["heap"] = ESP.getFreeHeap(); d["relay"] = digitalRead(RELAY_PIN);
    String s; serializeJson(d, s); r->send(200, "application/json", s);
  });
  server.begin();
  heap("after wifi ap+web");

  NimBLEDevice::init("");
  heap("after nimble init");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  Serial.println("BLE scan 15s...");
  NimBLEScanResults res = scan->getResults(15000, false);
  bool found = false;
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = res.getDevice(i);
    String mac = d->getAddress().toString().c_str();
    if (mac.equalsIgnoreCase(DONGLE_MAC)) {
      found = true;
      Serial.printf("DONGLE FOUND rssi=%d name='%s'\n", d->getRSSI(), d->getName().c_str());
    }
  }
  Serial.printf("BLE scan: %d devices, dongle %s\n", res.getCount(), found ? "FOUND" : "not seen");
  heap("after ble scan");
  if (found) {
    NimBLEClient* c = NimBLEDevice::createClient();
    if (c->connect(NimBLEAddress(DONGLE_MAC, BLE_ADDR_PUBLIC))) {
      heap("ble connected");
      Serial.printf("connected, mtu=%d\n", c->getMTU());
      c->disconnect();
    } else Serial.println("connect FAILED");
  }
  Serial.println("=== spike done ===");
}

void loop() {
  static uint32_t t = 0;
  if (millis() - t > 10000) { t = millis(); heap("loop"); }
}
