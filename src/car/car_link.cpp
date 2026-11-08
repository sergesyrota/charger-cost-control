// NimBLE central to the Veepeak BLE OBD dongle (PLAN §8.1). Owner: car.
// One client object is created once and reused; every poll is connect -> commands -> disconnect,
// so no GATT session (and its RAM) outlives a poll. The whole session holds netlock.
//
// TODO(§8.5, milestone 6): cell-voltage capture (96 cells at SOC 9–11 % / 94–99 %, once per 24 h per
// bucket, discard if |ΔI| > 10 %). obd::cellPid()/cellVolts() are ready; needs a storage contract.
#include "car/car_link.h"

#include "app/wdt.h"

#include <Arduino.h>
#include <NimBLEDevice.h>

#include <atomic>

#include "car/car_status.h"
#include "car/elm_parse.h"
#include "car/obd_pids.h"
#include "net/clock.h"
#include "net/net_lock.h"

namespace carlink {

namespace {

constexpr uint32_t kConnectTimeoutMs = 15000;
constexpr uint32_t kCmdTimeoutMs = 10000;
constexpr uint32_t kLockTimeoutMs = 60000;
const char* const kNotifyUuid = "0000fff1-0000-1000-8000-00805f9b34fb";
const char* const kWriteUuid = "0000fff2-0000-1000-8000-00805f9b34fb";

std::atomic<bool> s_busy{false};
bool s_inited = false;
char s_mac[18] = "8C:DE:52:DE:B4:A0";
NimBLEClient* s_client = nullptr;

// Notification accumulator (filled from the NimBLE host task).
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
char s_rx[384];
volatile size_t s_rx_len = 0;
volatile bool s_rx_done = false;

void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  portENTER_CRITICAL(&s_mux);
  for (size_t i = 0; i < len; i++) {
    if (s_rx_len < sizeof(s_rx)) s_rx[s_rx_len++] = (char)data[i];
    if (data[i] == '>') s_rx_done = true;
  }
  portEXIT_CRITICAL(&s_mux);
}

void rxReset() {
  portENTER_CRITICAL(&s_mux);
  s_rx_len = 0;
  s_rx_done = false;
  portEXIT_CRITICAL(&s_mux);
}

bool waitPrompt(uint32_t timeout_ms) {
  uint32_t t0 = millis();
  while (!s_rx_done) {
    if (millis() - t0 >= timeout_ms) return false;
    if (s_client && !s_client->isConnected()) return false;
    delay(10);
    wdtFeed();
  }
  return true;
}

// One connected ELM session.
struct Session {
  NimBLERemoteCharacteristic* wr = nullptr;
  char hdr[4] = "";  // header last sent on this connection

  bool send(const char* text) {
    rxReset();
    return wr->writeValue(reinterpret_cast<const uint8_t*>(text), strlen(text), false);
  }

  obd::ElmResponse cmd(const obd::Pid& pid) {
    obd::ElmResponse r;
    r.status = obd::ElmStatus::Timeout;
    char line[16];
    if (strcmp(hdr, pid.header) != 0) {
      snprintf(line, sizeof(line), "AT SH %s\r", pid.header);
      if (!send(line)) return r;
      waitPrompt(kCmdTimeoutMs);  // like the Python: timeout here is only logged
      strlcpy(hdr, pid.header, sizeof(hdr));
    }
    snprintf(line, sizeof(line), "%s\r", pid.cmd);
    if (!send(line)) return r;
    bool done = waitPrompt(kCmdTimeoutMs);
    char buf[sizeof(s_rx)];
    size_t n;
    portENTER_CRITICAL(&s_mux);
    n = s_rx_len;
    memcpy(buf, s_rx, n);
    portEXIT_CRITICAL(&s_mux);
    // Python parses whatever arrived even after a timeout.
    r = obd::parseResponse(buf, n, pid.cmd);
    if (!done && r.status == obd::ElmStatus::Malformed) r.status = obd::ElmStatus::Timeout;
    return r;
  }
};

NimBLERemoteCharacteristic* findChar(const char* uuid) {
  NimBLERemoteService* svc = s_client->getService("fff0");
  if (svc) {
    if (auto* c = svc->getCharacteristic(uuid)) return c;
  }
  for (auto* s : s_client->getServices(true)) {
    if (auto* c = s->getCharacteristic(uuid)) return c;
  }
  return nullptr;
}

// True while the link exists in any form. NOT isConnected(): NimBLEClient::disconnect() flips the
// status to DISCONNECTING at once, so isConnected() goes false before the link is down. The handle
// resets only on the real disconnect event.
bool linkUp() { return s_client && s_client->getConnHandle() != BLE_HS_CONN_HANDLE_NONE; }

void disconnect() {
  if (!s_client) return;
  if (linkUp()) {
    s_client->disconnect();
    uint32_t t0 = millis();
    while (linkUp() && millis() - t0 < 3000) { delay(10); wdtFeed(); }
  }
  s_client->deleteServices();  // free discovered attribute tables
}

const char* errName(obd::ElmStatus s) {
  switch (s) {
    case obd::ElmStatus::Timeout: return "таймаут";
    case obd::ElmStatus::Malformed: return "неверный ответ";
    case obd::ElmStatus::CommandError: return "ERROR";
    default: return "";
  }
}

}  // namespace

void begin() {
  if (s_inited) return;
  NimBLEDevice::init("");
  s_client = NimBLEDevice::createClient();
  // NimBLE's client table survives deinit(); a client deinit() couldn't delete still holds the only
  // slot. Reuse it rather than crash on nullptr.
  if (!s_client) s_client = NimBLEDevice::getDisconnectedClient();
  if (!s_client) {
    Serial.println("[car] no BLE client available");
    return;
  }
  s_client->setConnectTimeout(kConnectTimeoutMs);
  s_inited = true;
}

void end() {
  if (!s_inited || s_busy.load()) return;
  disconnect();
  if (linkUp()) {  // deinit() now would leak the client (deleteClient refuses a live link)
    Serial.println("[car] BLE link still up, keeping the stack until the next end()");
    return;
  }
  NimBLEDevice::deinit(true);  // frees the client and the host stack (OTA needs the RAM)
  s_client = nullptr;
  s_inited = false;
}

void setMac(const char* mac) {
  if (mac && strlen(mac) == 17) strlcpy(s_mac, mac, sizeof(s_mac));
}

bool busy() { return s_busy.load(); }

bool poll(CarSnapshot& io, const PollOpts& opts, int64_t now) {
  if (!s_inited) begin();
  io.last_poll_at = now;
  io.last_poll_up_s = sysclock::uptimeS();  // 64-bit esp_timer: no 49.7-day millis() wrap
  io.error[0] = 0;
  if (!s_inited) {
    strlcpy(io.error, "ошибка BLE", sizeof(io.error));
    return false;
  }

  netlock::Guard lock("car", kLockTimeoutMs);
  if (!lock.ok()) {
    strlcpy(io.error, "сеть занята", sizeof(io.error));
    return false;
  }
  s_busy = true;

  bool connected = s_client->connect(NimBLEAddress(std::string(s_mac), BLE_ADDR_PUBLIC), true);
  if (!connected) {
    disconnect();
    io.status = CarStatus::NotDetected;
    io.soc_live = false;
    io.volts = io.amps = io.watts = NAN;
    strlcpy(io.error, "адаптер не найден", sizeof(io.error));
    s_busy = false;
    return false;
  }

  Session ses;
  NimBLERemoteCharacteristic* nt = findChar(kNotifyUuid);
  ses.wr = findChar(kWriteUuid);
  if (!nt || !ses.wr || !nt->subscribe(true, onNotify)) {
    disconnect();
    // Connected to the dongle, but can't talk ELM: treat like no car answer.
    io.status = CarStatus::NotDetected;
    io.soc_live = false;
    io.volts = io.amps = io.watts = NAN;
    strlcpy(io.error, "ошибка настройки GATT адаптера", sizeof(io.error));
    s_busy = false;
    return false;
  }

  obd::ElmResponse soc = ses.cmd(obd::kSoc);
  float watts = NAN;
  io.volts = io.amps = io.watts = NAN;
  bool online = soc.status == obd::ElmStatus::Ok || soc.status == obd::ElmStatus::NoData;
  if (soc.status == obd::ElmStatus::Ok) {
    float v = obd::socPct(soc);
    if (!isnan(v)) {
      io.soc = v;
      io.soc_at = now;
      io.soc_live = true;
    } else {
      io.soc_live = false;
    }
  } else {
    io.soc_live = false;
    if (!online && soc.status != obd::ElmStatus::CommandError)  // ERROR = asleep, normal
      snprintf(io.error, sizeof(io.error), "заряд: %s", errName(soc.status));
  }

  if (online) {
    obd::ElmResponse rv = ses.cmd(obd::kPackVolts);
    obd::ElmResponse ri = ses.cmd(obd::kPackAmps);
    io.volts = obd::packVolts(rv);
    io.amps = obd::packAmps(ri);
    if (!isnan(io.volts) && !isnan(io.amps)) watts = io.volts * io.amps;
    io.watts = watts;
    if (isnan(watts)) strlcpy(io.error, "не удалось прочитать мощность", sizeof(io.error));

    if (opts.extras) {
      float ah = obd::capacityAh(ses.cmd(obd::kCapacity));
      if (!isnan(ah)) io.capacity_kwh = obd::capacityKwh(ah);
      int32_t cc = obd::chargeCount(ses.cmd(obd::kChargeCount));
      if (cc >= 0) io.charge_count = cc;
      float odo = obd::odometerMi(ses.cmd(obd::kOdometer));
      if (!isnan(odo)) io.odometer_mi = odo;
    }
  }

  nt->unsubscribe();
  disconnect();

  io.status = obd::classify(true, soc.status, watts, opts.threshold_w, opts.power_sign);
  io.last_seen = now;
  s_busy = false;
  return true;
}

}  // namespace carlink
