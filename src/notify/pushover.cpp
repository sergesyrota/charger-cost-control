// Pushover notifier. Owner: notify_ota.
#include "notify/pushover.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <string>

#include "net/https_client.h"
#include "store/event_log.h"

namespace pushover {

static const char* kUrl = "https://api.pushover.net/1/messages.json";
// Retries after a real send failure (HTTP/TLS): 1, 5, 15 min, then dropped. "No network",
// "clock not set" and "network busy" don't count: the message waits until it can be tried.
static constexpr uint32_t kRetryMs[] = {60000, 300000, 900000};
static constexpr uint8_t kMaxTries = sizeof(kRetryMs) / sizeof(kRetryMs[0]);
static constexpr uint32_t kWaitMs = 30000;
static constexpr size_t kMsgMax = 300;

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_enabled = false;
static char s_token[40] = "", s_user[40] = "", s_device[32] = "";

struct Slot {
  bool used = false;
  uint8_t tries = 0;          // failed real attempts
  uint32_t seq = 0;
  uint32_t due_ms = 0;
  char msg[kMsgMax] = "";
};
static Slot s_q[2];
static uint32_t s_seq = 0;

static void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

void configure(const Creds& c) {
  portENTER_CRITICAL(&s_mux);
  s_enabled = c.enabled;
  copyStr(s_token, sizeof(s_token), c.token);
  copyStr(s_user, sizeof(s_user), c.user);
  copyStr(s_device, sizeof(s_device), c.device);
  portEXIT_CRITICAL(&s_mux);
}

bool ready() {
  portENTER_CRITICAL(&s_mux);
  bool r = s_enabled && s_token[0] && s_user[0];
  portEXIT_CRITICAL(&s_mux);
  return r;
}

bool queue(const char* msg) {
  if (!msg || !ready()) return false;
  portENTER_CRITICAL(&s_mux);
  if (s_q[0].used && s_q[1].used) {  // full: drop the oldest
    s_q[0] = s_q[1];
    s_q[1].used = false;
  }
  Slot& s = s_q[0].used ? s_q[1] : s_q[0];
  s.used = true;
  s.tries = 0;
  s.seq = ++s_seq;
  s.due_ms = millis();
  copyStr(s.msg, sizeof(s.msg), msg);
  portEXIT_CRITICAL(&s_mux);
  return true;
}

static void setErr(char* err, uint16_t cap, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void setErr(char* err, uint16_t cap, const char* fmt, ...) {
  if (!err || !cap) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, cap, fmt, ap);
  va_end(ap);
}

bool sendNow(const char* msg, char* err, uint16_t err_cap) {
  if (err && err_cap) err[0] = 0;
  if (!msg) msg = "";
  char token[40], user[40], device[32];
  portENTER_CRITICAL(&s_mux);
  bool ok = s_enabled && s_token[0] && s_user[0];
  memcpy(token, s_token, sizeof(token));
  memcpy(user, s_user, sizeof(user));
  memcpy(device, s_device, sizeof(device));
  portEXIT_CRITICAL(&s_mux);
  if (!ok) {
    setErr(err, err_cap, "Pushover not configured");
    return false;
  }

  std::string form;
  form.reserve(128 + strlen(msg) * 3);
  auto sink = [&form](const char* p, size_t n) { form.append(p, n); };
  form += "token=";
  https::urlEncodeAppend(token, sink);
  form += "&user=";
  https::urlEncodeAppend(user, sink);
  if (device[0]) {
    form += "&device=";
    https::urlEncodeAppend(device, sink);
  }
  form += "&message=";
  https::urlEncodeAppend(msg, sink);
  memset(token, 0, sizeof(token));
  memset(user, 0, sizeof(user));

  // Keep the head of the response to extract Pushover's error text.
  char body[200];
  size_t blen = 0;
  https::Result r = https::postForm(kUrl, form.c_str(), [&](const uint8_t* d, size_t n) {
    size_t room = sizeof(body) - 1 - blen;
    size_t take = n < room ? n : room;
    memcpy(body + blen, d, take);
    blen += take;
    return true;
  });
  body[blen] = 0;
  if (!form.empty()) memset(&form[0], 0, form.size());  // wipe secret-bearing buffer

  if (r.status == 200 && !*r.error) return true;
  if (r.status <= 0) {
    setErr(err, err_cap, "%s", *r.error ? r.error : "transport error");
    return false;
  }
  // Pushover: {"errors":["application token is invalid"],"status":0,...}
  const char* e = strstr(body, "\"errors\":[\"");
  if (e) {
    e += 11;
    const char* end = strchr(e, '"');
    int n = end ? (int)(end - e) : (int)strlen(e);
    if (n > 60) n = 60;
    setErr(err, err_cap, "HTTP %d: %.*s", r.status, n, e);
  } else {
    setErr(err, err_cap, "HTTP %d%s%s", r.status, *r.error ? " " : "", r.error);
  }
  return false;
}

void loop() {
  static Slot job;  // ~300 B: keep off the loop-task stack
  bool have = false;
  uint32_t now = millis();
  portENTER_CRITICAL(&s_mux);
  for (int i = 0; i < 2 && !have; i++) {
    if (s_q[i].used && (int32_t)(now - s_q[i].due_ms) >= 0) {
      job = s_q[i];
      have = true;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  if (!have) return;

  char err[80] = "";
  bool ok = sendNow(job.msg, err, sizeof(err));
  bool configured = ready();

  const bool waiting = !ok && (strcmp(err, "no network") == 0 || strcmp(err, "clock not set") == 0 ||
                                strcmp(err, "network busy") == 0);
  uint32_t retry_ms = 0;
  portENTER_CRITICAL(&s_mux);
  // queue() may have shifted/dropped slots while we were sending: find ours by sequence number.
  for (int i = 0; i < 2; i++) {
    Slot& s = s_q[i];
    if (!s.used || s.seq != job.seq) continue;
    if (ok || !configured || (!waiting && job.tries >= kMaxTries)) {
      s.used = false;
    } else if (waiting) {
      s.due_ms = millis() + kWaitMs;
      retry_ms = kWaitMs;
    } else {
      retry_ms = kRetryMs[s.tries];
      s.tries++;
      s.due_ms = millis() + retry_ms;
    }
  }
  if (!s_q[0].used && s_q[1].used) {
    s_q[0] = s_q[1];
    s_q[1].used = false;
  }
  portEXIT_CRITICAL(&s_mux);

  if (ok) evlog::add(evlog::Kind::Push, "Sent: %s", job.msg);
  else if (waiting) return;  // quiet: re-tried every 30 s until the network/clock is back
  else if (retry_ms) evlog::add(evlog::Kind::Err, "Pushover failed (%s), retry in %u min", err, (unsigned)(retry_ms / 60000));
  else evlog::add(evlog::Kind::Err, "Pushover failed (%s), dropped", err);
}

// ---------------------------------------------------------------- formatting

namespace {
struct Out {
  char* p;
  size_t cap;
  size_t len;
  void add(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(p + len, cap - len, fmt, ap);
    va_end(ap);
    if (n > 0) len += (size_t)n;
    if (len >= cap) len = cap - 1;
  }
};

void addReason(Out& o, const Decision& d) {  // without trailing '.'/' '
  size_t n = strnlen(d.reason, sizeof(d.reason));
  while (n && (d.reason[n - 1] == '.' || d.reason[n - 1] == ' ')) n--;
  o.add("%.*s", (int)n, d.reason);
}

void addSoc(Out& o, float soc) { o.add("%d%%", (int)lroundf(soc)); }

void addAge(Out& o, int64_t secs) {
  if (secs < 60) o.add("just now");
  else if (secs < 3600) o.add("%dm ago", (int)(secs / 60));
  else if (secs < 2 * 86400) o.add("%dh ago", (int)(secs / 3600));
  else o.add("%dd ago", (int)(secs / 86400));
}
}  // namespace

void formatOn(const Decision& d, bool manual, const CarSnapshot& car, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  Out o{out, cap, 0};
  o.add("%sON: ", manual ? "Manual: " : "");
  addReason(o, d);
  if (d.src == DecisionSrc::Price && d.state[0]) {
    o.add(" — Bolt/%s", d.state);
    if (!isnan(d.block_soc)) o.add(" soc≥%g", (double)d.block_soc);
  }
  o.add(". ");
  bool soc = !isnan(car.soc) && car.soc_live;
  switch (car.status) {
    case CarStatus::Charging:
      if (!isnan(car.watts)) o.add("Charging started: %.1f kW", fabsf(car.watts) / 1000.0f);
      else o.add("Charging started");
      if (soc) {
        o.add(", ");
        addSoc(o, car.soc);
      }
      o.add(".");
      break;
    case CarStatus::Online:
    case CarStatus::Asleep:
      o.add("Car connected, not charging");
      if (soc) {
        o.add(" (");
        addSoc(o, car.soc);
        o.add(")");
      }
      o.add(".");
      break;
    default:
      o.add("Car not detected.");
      break;
  }
}

void formatOff(const Decision& d, bool manual, const CarSnapshot& car, int64_t now, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  Out o{out, cap, 0};
  o.add("%sOFF: ", manual ? "Manual: " : "");
  addReason(o, d);
  if (d.src == DecisionSrc::Price) {
    if (d.state[0]) o.add(", Bolt/%s", d.state);
    if (!isnan(car.soc)) {
      o.add(" (");
      addSoc(o, car.soc);
      if (now > 0 && car.soc_at > 0 && now >= car.soc_at) {
        o.add(", ");
        addAge(o, now - car.soc_at);
      }
      o.add(")");
    }
  }
  o.add(".");
}

}  // namespace pushover
