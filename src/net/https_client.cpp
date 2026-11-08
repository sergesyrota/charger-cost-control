// Minimal HTTPS client over NetworkClientSecure + HTTPClient. Owner: net.
//
// - Built-in CA bundle (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE, full set) validates every host.
// - Redirects are followed manually (max 5, https only): each hop closes the previous TLS session
//   before opening the next, so there is never more than one TLS session alive.
// - get(): the body is streamed only for 2xx (keeps price/manifest parsers away from error pages);
//   on_start always sees the final status. postForm(): body streamed for any status (Pushover errors).
// - Refuses to run without a valid clock (certificate dates) or without a STA connection.
#include "net/https_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include <memory>

#include "app/wdt.h"
#include "net/clock.h"
#include "net/net_lock.h"
#include "net/wifi_mgr.h"

namespace https {

const char* const kErrNoNetwork = "нет сети";
const char* const kErrNoClock = "часы не установлены";
const char* const kErrBusy = "сеть занята";
const char* const kErrTransport = "ошибка соединения";
const char* const kErrConnect = "нет соединения с сервером";

namespace {

constexpr int kMaxRedirects = 5;
// Each unfed blocking phase (TCP connect, TLS handshake, header wait / per-read) is capped so their
// sum stays well under the loop task's 60 s watchdog (app.cpp). The body stream feeds the WDT.
constexpr uint32_t kPhaseCapMs = 15000;

void logHeap(const char* when, const char* url) {
#if CORE_DEBUG_LEVEL >= 4
  log_d("https %s %s: free=%u largest=%u", when, url, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#else
  (void)when;
  (void)url;
#endif
}

const char* precheck(const char* url) {
  if (!url || strncmp(url, "https://", 8) != 0) return "только https";
  if (!sysclock::valid()) return kErrNoClock;
  if (!wifi::staConnected()) return kErrNoNetwork;
  return nullptr;
}

bool isRedirect(int code) { return code == 301 || code == 302 || code == 303 || code == 307 || code == 308; }

// Resolve a Location header against the current URL. Returns false if the result is not https.
bool resolveLocation(const String& cur, const String& loc, String& out) {
  if (loc.startsWith("https://")) {
    out = loc;
    return true;
  }
  if (loc.startsWith("/")) {
    int slash = cur.indexOf('/', 8);
    out = (slash < 0 ? cur : cur.substring(0, slash)) + loc;
    return true;
  }
  return false;  // http:// or relative-path redirects are not supported
}

// Own body loop (HTTP/1.0 => no chunked encoding; server closes at the end). Never spins: sleeps
// 5 ms when no data, gives up after `idle_ms` without data or `total_ms` overall.
const char* streamBody(HTTPClient& http, const BodyCb& body, Result& r, uint32_t idle_ms, uint32_t total_ms) {
  NetworkClient* s = http.getStreamPtr();
  if (!s) return "ошибка чтения";
  static uint8_t buf[512];  // loop task only (under netlock)
  int remaining = http.getSize();  // -1 = until close
  const uint32_t t0 = millis();
  uint32_t last = t0;
  while (remaining != 0) {
    if (millis() - t0 > total_ms) return "таймаут чтения";
    int a = s->available();
    if (a > 0) {
      size_t want = (size_t)a < sizeof(buf) ? (size_t)a : sizeof(buf);
      if (remaining > 0 && (size_t)remaining < want) want = remaining;
      int n = s->read(buf, want);
      if (n <= 0) { delay(5); continue; }
      last = millis();
      wdtFeed();
      if (body && !body(buf, (size_t)n)) return "прервано";
      r.bytes += n;
      if (remaining > 0) remaining -= n;
    } else {
      if (!s->connected()) break;
      if (millis() - last > idle_ms) return "таймаут чтения";
      delay(5);
    }
  }
  if (remaining > 0) return "ответ оборван";
  return "";
}

// One request including redirect handling. `form` == nullptr => GET.
Result run(const char* url, const char* form, const BodyCb& body, uint32_t timeout_ms, const StartCb& on_start) {
  Result r;
  if (const char* e = precheck(url)) {
    r.status = -1;
    r.error = e;
    return r;
  }
  netlock::Guard lock("https", timeout_ms + 5000);
  if (!lock.ok()) {
    r.status = -1;
    r.error = kErrBusy;
    return r;
  }

  String cur(url);
  static const char* kHeaders[] = {"Location"};
  for (int hop = 0; hop <= kMaxRedirects; hop++) {
    wdtFeed();
    logHeap("before", cur.c_str());
    // Heap-allocated: keeps the loop task's stack small (TLS handshake needs it).
    std::unique_ptr<NetworkClientSecure> client(new NetworkClientSecure());
    client->useBuiltinCACertBundle();
    const uint32_t phase_ms = timeout_ms < kPhaseCapMs ? timeout_ms : kPhaseCapMs;
    client->setTimeout((phase_ms + 999) / 1000);
    client->setHandshakeTimeout((phase_ms + 999) / 1000);  // library default is 120 s
    std::unique_ptr<HTTPClient> http(new HTTPClient());
    http->setReuse(false);
    http->useHTTP10(true);  // no chunked bodies; our streamBody reads until close/length
    http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http->setConnectTimeout(phase_ms);
    http->setTimeout((uint16_t)phase_ms);
    http->setUserAgent("ChargerControl/" FW_VERSION);
    if (!http->begin(*client, cur)) {
      r.status = -1;
      r.error = "неверный адрес";
      return r;
    }
    http->collectHeaders(kHeaders, 1);
    int code;
    if (form) {
      http->addHeader("Content-Type", "application/x-www-form-urlencoded");
      code = http->POST((uint8_t*)form, strlen(form));
    } else {
      code = http->GET();
    }
    wdtFeed();
    if (code <= 0) {
      http->end();
      logHeap("after", cur.c_str());
      r.status = code < 0 ? code : -1;
      r.error = kErrConnect;
      return r;
    }
    if (!form && isRedirect(code)) {
      String next;
      bool ok = resolveLocation(cur, http->header("Location"), next);
      http->end();
      client->stop();
      logHeap("after", cur.c_str());
      if (!ok) {
        r.status = code;
        r.error = "неверное перенаправление";
        return r;
      }
      cur = next;
      continue;
    }
    r.status = code;
    r.content_length = http->getSize();
    if (on_start) on_start(code, r.content_length);
    if (r.content_length != 0 && (form || (code >= 200 && code < 300))) r.error = streamBody(*http, body, r, phase_ms, timeout_ms);
    http->end();
    logHeap("after", cur.c_str());
    return r;
  }
  r.status = -1;
  r.error = "слишком много перенаправлений";
  return r;
}

}  // namespace

// One retry after a short pause when the connection/handshake failed before any body byte arrived.
// Back-to-back TLS sessions (e.g. OTA check then install) can fail the RSA-4096 certificate check
// with an MPI allocation error while the previous session's memory is still being released.
Result runRetry(const char* url, const char* form, const BodyCb& body, uint32_t timeout_ms, const StartCb& on_start) {
  Result r = run(url, form, body, timeout_ms, on_start);
  if (r.status < 0 && r.bytes == 0 && r.error == kErrConnect) {
    for (int i = 0; i < 30; i++) { wdtFeed(); delay(100); }
    r = run(url, form, body, timeout_ms, on_start);
  }
  return r;
}

Result get(const char* url, const BodyCb& body, uint32_t timeout_ms, const StartCb& on_start) {
  return runRetry(url, nullptr, body, timeout_ms, on_start);
}

Result postForm(const char* url, const char* form, const BodyCb& body, uint32_t timeout_ms) {
  return runRetry(url, form ? form : "", body, timeout_ms, nullptr);
}

void urlEncodeAppend(const char* value, std::function<void(const char*, size_t)> sink) {
  if (!value || !sink) return;
  static const char kHex[] = "0123456789ABCDEF";
  char buf[48];
  size_t n = 0;
  for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
    if (n > sizeof(buf) - 3) {
      sink(buf, n);
      n = 0;
    }
    unsigned char c = *p;
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      buf[n++] = (char)c;
    } else {
      buf[n++] = '%';
      buf[n++] = kHex[c >> 4];
      buf[n++] = kHex[c & 15];
    }
  }
  if (n) sink(buf, n);
}

}  // namespace https
