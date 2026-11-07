// Pushover notifier (PLAN §10). POST https://api.pushover.net/1/messages.json (token, user,
// message, optional device) via https::postForm. Owner: notify_ota.
// Policy (enforced by app, not here): only relay transitions, never repeats.
// Transport failure: evlog Err, retry ONCE after 60 s, then drop. Non-blocking queue of at most 2
// messages: queuing when full drops the oldest pending one (including a pending retry).
// Secrets (token/user) are never logged or returned.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core_types.h"

namespace pushover {

struct Creds {
  bool enabled;
  const char* token;
  const char* user;
  const char* device;  // "" = all devices
};

void configure(const Creds& c);           // copies strings; call after every config change
bool ready();                             // enabled && token && user set

// Queue a message; sent from loop(). Returns false (and does nothing) if !ready().
bool queue(const char* msg);
// Blocking send now (used by the "Send test" job). Returns true on HTTP 200; err gets a short reason.
bool sendNow(const char* msg, char* err, uint16_t err_cap);
void loop();                              // loop task: sends queued message / handles retry

// ---- message formatting (PLAN §10). `out` is always NUL-terminated.
// ON (call after the post-ON car probe), e.g.
//   "ON: Price is low (1.2¢ ≤ 2¢) — Bolt/Заряд. Charging started: 1.4 kW, 52%."
//   "… Car connected, not charging (52%)."  /  "… Car not detected."
// `manual` prefixes "Manual: ". "Bolt/<state>" is appended only for price decisions.
void formatOn(const Decision& d, bool manual, const CarSnapshot& car, char* out, size_t cap);
// OFF, e.g. "OFF: Price is high (3.4¢ > 3¢), Bolt/Заряд (52%, 5m ago)." / "OFF: Bolt is full."
// `now` = epoch (0 if no clock: the age is omitted).
void formatOff(const Decision& d, bool manual, const CarSnapshot& car, int64_t now, char* out, size_t cap);

}  // namespace pushover
