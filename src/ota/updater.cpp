// HTTPS OTA + rollback support. Owner: notify_ota.
#include "ota/updater.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <mbedtls/sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "core_types.h"
#include "net/https_client.h"
#include "net/net_lock.h"
#include "store/event_log.h"

// Arduino core (esp32-hal-misc.c) marks the app valid at boot unless this returns true. We validate
// ourselves after the health checks (PLAN §12).
extern "C" bool verifyRollbackLater() { return true; }

namespace ota {

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static Progress s_p;  // guarded by s_mux

const char* phaseName(Phase p) {
  static const char* const n[] = {"idle", "checking", "available", "up_to_date", "downloading", "verifying", "done", "error"};
  return (uint8_t)p < 8 ? n[(int)p] : "error";
}

Progress progress() {
  Progress out;
  portENTER_CRITICAL(&s_mux);
  out = s_p;
  portEXIT_CRITICAL(&s_mux);
  return out;
}

static void setPhase(Phase ph) {
  portENTER_CRITICAL(&s_mux);
  s_p.phase = ph;
  if (ph != Phase::Error) s_p.error[0] = 0;
  portEXIT_CRITICAL(&s_mux);
}

static bool fail(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static bool fail(const char* fmt, ...) {
  char e[80];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e, sizeof(e), fmt, ap);
  va_end(ap);
  portENTER_CRITICAL(&s_mux);
  s_p.phase = Phase::Error;
  memcpy(s_p.error, e, sizeof(e));
  portEXIT_CRITICAL(&s_mux);
  evlog::add(evlog::Kind::Err, "OTA: %s", e);
  return false;
}

static bool isHttps(const char* u) { return u && strncmp(u, "https://", 8) == 0 && u[8]; }

static bool endsWithBin(const char* u) {
  const char* q = strpbrk(u, "?#");
  size_t n = q ? (size_t)(q - u) : strlen(u);
  return n >= 4 && strncasecmp(u + n - 4, ".bin", 4) == 0;
}

static void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

static bool isHex64(const char* s) {
  for (int i = 0; i < 64; i++)
    if (!isxdigit((unsigned char)s[i])) return false;
  return s[64] == 0;
}

int compareVersions(const char* a, const char* b, bool* ok) {
  auto parse = [](const char* s, long v[3]) -> bool {
    v[0] = v[1] = v[2] = 0;
    if (!s) return false;
    if (*s == 'v' || *s == 'V') s++;
    if (!isdigit((unsigned char)*s)) return false;
    for (int i = 0; i < 3; i++) {
      char* end;
      v[i] = strtol(s, &end, 10);
      s = end;
      if (*s != '.' || !isdigit((unsigned char)s[1])) break;
      s++;
    }
    return true;
  };
  long va[3], vb[3];
  bool good = parse(a, va) & parse(b, vb);
  if (ok) *ok = good;
  for (int i = 0; i < 3; i++)
    if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
  return 0;
}

bool check(const char* url) {
  if (!isHttps(url)) return fail("URL must be https://");
  setPhase(Phase::Checking);

  static Manifest m;  // keep off the stack
  m = Manifest();

  if (endsWithBin(url)) {
    if (strlen(url) >= sizeof(m.url)) return fail("URL too long");
    copyStr(m.url, sizeof(m.url), url);
    m.direct_bin = true;
  } else {
    static char buf[1024];
    size_t len = 0;
    bool overflow = false;
    https::Result r = https::get(url, [&](const uint8_t* d, size_t n) {
      if (len + n >= sizeof(buf)) {
        overflow = true;
        return false;
      }
      memcpy(buf + len, d, n);
      len += n;
      return true;
    });
    buf[len] = 0;
    if (overflow) return fail("Manifest too large");
    if (!r.ok()) {
      if (r.status > 0) return fail("Manifest HTTP %d", r.status);
      return fail("Manifest fetch: %s", *r.error ? r.error : "transport error");
    }
    JsonDocument doc;
    if (deserializeJson(doc, buf, len)) return fail("Manifest is not valid JSON");
    const char* ver = doc["version"] | "";
    const char* bin = doc["url"] | "";
    const char* sha = doc["sha256"] | "";
    const char* notes = doc["notes"] | "";
    uint32_t size = doc["size"] | 0u;
    if (!*ver || strlen(ver) >= sizeof(m.version)) return fail("Manifest: bad version");
    if (!isHttps(bin) || strlen(bin) >= sizeof(m.url)) return fail("Manifest: url must be https://");
    if (!isHex64(sha)) return fail("Manifest: bad sha256");
    copyStr(m.version, sizeof(m.version), ver);
    copyStr(m.url, sizeof(m.url), bin);
    for (int i = 0; i < 64; i++) m.sha256[i] = (char)tolower((unsigned char)sha[i]);
    m.sha256[64] = 0;
    m.size = size;
    copyStr(m.notes, sizeof(m.notes), notes);
  }

  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (!next) return fail("No OTA partition");
  if (m.size && m.size > next->size) return fail("Image too large (%u > %u)", (unsigned)m.size, (unsigned)next->size);

  Phase ph = Phase::Available;
  if (!m.direct_bin) {
    bool ok;
    int c = compareVersions(m.version, FW_VERSION, &ok);
    if (ok && c <= 0) ph = Phase::UpToDate;
  }
  portENTER_CRITICAL(&s_mux);
  s_p.manifest = m;
  s_p.has_manifest = true;
  s_p.phase = ph;
  s_p.bytes = 0;
  s_p.total = m.size;
  s_p.error[0] = 0;
  portEXIT_CRITICAL(&s_mux);
  if (m.direct_bin) evlog::add(evlog::Kind::Sys, "OTA: direct .bin (no sha256 check)");
  else evlog::add(evlog::Kind::Sys, "OTA: manifest %s (running %s) -> %s", m.version, FW_VERSION, phaseName(ph));
  return true;
}

namespace {
struct Dl {
  esp_ota_handle_t h = 0;
  mbedtls_sha256_context sha;
  uint32_t bytes = 0;
  uint32_t expect = 0;   // 0 unknown
  uint32_t cap = 0;      // partition size
  const char* err = nullptr;
};
}  // namespace

bool install(const char* url) {
  if (url && *url) {
    if (!check(url)) return false;
  }
  static Manifest m;
  portENTER_CRITICAL(&s_mux);
  bool have = s_p.has_manifest;
  if (have) m = s_p.manifest;
  portEXIT_CRITICAL(&s_mux);
  if (!have) return fail("No manifest checked");
  if (!isHttps(m.url)) return fail("URL must be https://");

  netlock::Guard lock("ota", 60000);
  if (!lock.ok()) return fail("Network busy");

  const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
  if (!part) return fail("No OTA partition");

  static Dl dl;
  dl = Dl();
  dl.expect = m.size;
  dl.cap = part->size;
  if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &dl.h) != ESP_OK) return fail("esp_ota_begin failed");
  mbedtls_sha256_init(&dl.sha);
  mbedtls_sha256_starts(&dl.sha, 0);

  portENTER_CRITICAL(&s_mux);
  s_p.phase = Phase::Downloading;
  s_p.bytes = 0;
  s_p.total = m.size;
  s_p.error[0] = 0;
  portEXIT_CRITICAL(&s_mux);
  evlog::add(evlog::Kind::Sys, "OTA: installing %s into %s", m.direct_bin ? "direct .bin" : m.version, part->label);

  https::Result r = https::get(
      m.url,
      [](const uint8_t* d, size_t n) {
        if (dl.err) return false;
        if (dl.bytes + n > dl.cap || (dl.expect && dl.bytes + n > dl.expect)) {
          dl.err = "Image larger than expected";
          return false;
        }
        if (esp_ota_write(dl.h, d, n) != ESP_OK) {
          dl.err = "Flash write failed";
          return false;
        }
        mbedtls_sha256_update(&dl.sha, d, n);
        dl.bytes += n;
        portENTER_CRITICAL(&s_mux);
        s_p.bytes = dl.bytes;
        portEXIT_CRITICAL(&s_mux);
        return true;
      },
      30000,
      [](int status, int32_t clen) {
        if (status >= 200 && status < 300 && clen > 0) {
          if (dl.expect && (uint32_t)clen != dl.expect) dl.err = "Size mismatch (Content-Length)";
          portENTER_CRITICAL(&s_mux);
          if (!s_p.total) s_p.total = (uint32_t)clen;
          portEXIT_CRITICAL(&s_mux);
          if (!dl.expect) dl.expect = (uint32_t)clen;
        }
      });

  uint8_t digest[32];
  mbedtls_sha256_finish(&dl.sha, digest);
  mbedtls_sha256_free(&dl.sha);

  if (dl.err || !r.ok()) {
    esp_ota_abort(dl.h);
    if (dl.err) return fail("%s", dl.err);
    if (r.status > 0 && (r.status < 200 || r.status >= 300)) return fail("Download HTTP %d", r.status);
    return fail("Download: %s", *r.error ? r.error : "transport error");
  }

  setPhase(Phase::Verifying);
  if (!dl.bytes || (dl.expect && dl.bytes != dl.expect)) {
    esp_ota_abort(dl.h);
    return fail("Size mismatch (%u of %u)", (unsigned)dl.bytes, (unsigned)dl.expect);
  }
  if (!m.direct_bin) {
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    if (strcmp(hex, m.sha256) != 0) {
      esp_ota_abort(dl.h);
      return fail("sha256 mismatch");
    }
  }
  esp_err_t e = esp_ota_end(dl.h);  // validates the image header/checksum
  if (e != ESP_OK) return fail("Image invalid (%s)", esp_err_to_name(e));
  e = esp_ota_set_boot_partition(part);
  if (e != ESP_OK) return fail("Set boot partition failed (%s)", esp_err_to_name(e));

  setPhase(Phase::Done);
  evlog::add(evlog::Kind::Sys, "OTA: %u bytes written to %s; reboot to apply", (unsigned)dl.bytes, part->label);
  return true;
}

bool runningFromRollback() { return esp_ota_get_last_invalid_partition() != nullptr; }

bool pendingVerify() {
  esp_ota_img_states_t st;
  const esp_partition_t* run = esp_ota_get_running_partition();
  return run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY;
}

void markValid() {
  if (!pendingVerify()) return;
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) evlog::add(evlog::Kind::Sys, "OTA: image %s marked valid", FW_VERSION);
}

void rollbackIfPending() {
  if (!pendingVerify()) return;
  evlog::add(evlog::Kind::Err, "OTA: health checks not passed in time; rolling back");
  delay(200);  // let the log line reach serial/flash
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

}  // namespace ota
