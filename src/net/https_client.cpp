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

#include "net/clock.h"
#include "net/net_lock.h"
#include "net/wifi_mgr.h"

namespace https {

namespace {

constexpr int kMaxRedirects = 5;

// Write-only Stream that forwards to the body callback; returns 0 on abort so writeToStream stops.
class CbSink : public Stream {
 public:
  explicit CbSink(const BodyCb& cb) : cb_(cb) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t len) override {
    if (aborted_) return 0;
    if (cb_ && !cb_(buf, len)) {
      aborted_ = true;
      return 0;
    }
    bytes_ += len;
    return len;
  }
  bool aborted() const { return aborted_; }
  size_t bytes() const { return bytes_; }

 private:
  const BodyCb& cb_;
  bool aborted_ = false;
  size_t bytes_ = 0;
};

void logHeap(const char* when, const char* url) {
#if CORE_DEBUG_LEVEL >= 4
  log_d("https %s %s: free=%u largest=%u", when, url, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
#else
  (void)when;
  (void)url;
#endif
}

const char* precheck(const char* url) {
  if (!url || strncmp(url, "https://", 8) != 0) return "https only";
  if (!sysclock::valid()) return "clock not set";
  if (!wifi::staConnected()) return "no network";
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

const char* streamBody(HTTPClient& http, const BodyCb& body, Result& r) {
  CbSink sink(body);
  int n = http.writeToStream(&sink);
  r.bytes = sink.bytes();
  if (sink.aborted()) return "aborted";
  if (n == HTTPC_ERROR_NOT_CONNECTED && sink.bytes() == 0) return "";  // empty body, server closed
  if (n < 0) return "read failed";
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
    r.error = "network busy";
    return r;
  }

  String cur(url);
  static const char* kHeaders[] = {"Location"};
  for (int hop = 0; hop <= kMaxRedirects; hop++) {
    logHeap("before", cur.c_str());
    // Heap-allocated: keeps the loop task's stack small (TLS handshake needs it).
    std::unique_ptr<NetworkClientSecure> client(new NetworkClientSecure());
    client->useBuiltinCACertBundle();
    client->setTimeout((timeout_ms + 999) / 1000);
    std::unique_ptr<HTTPClient> http(new HTTPClient());
    http->setReuse(false);
    http->setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http->setConnectTimeout(timeout_ms);
    http->setTimeout(timeout_ms > 65535 ? 65535 : (uint16_t)timeout_ms);
    http->setUserAgent("ChargerControl/" FW_VERSION);
    if (!http->begin(*client, cur)) {
      r.status = -1;
      r.error = "bad url";
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
    if (code <= 0) {
      http->end();
      logHeap("after", cur.c_str());
      r.status = code < 0 ? code : -1;
      r.error = "connect failed";
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
        r.error = "bad redirect";
        return r;
      }
      cur = next;
      continue;
    }
    r.status = code;
    r.content_length = http->getSize();
    if (on_start) on_start(code, r.content_length);
    if (r.content_length != 0 && (form || (code >= 200 && code < 300))) r.error = streamBody(*http, body, r);
    http->end();
    logHeap("after", cur.c_str());
    return r;
  }
  r.status = -1;
  r.error = "too many redirects";
  return r;
}

}  // namespace

Result get(const char* url, const BodyCb& body, uint32_t timeout_ms, const StartCb& on_start) {
  return run(url, nullptr, body, timeout_ms, on_start);
}

Result postForm(const char* url, const char* form, const BodyCb& body, uint32_t timeout_ms) {
  return run(url, form ? form : "", body, timeout_ms, nullptr);
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
