// Pushover notifier (PLAN §10). POST https://api.pushover.net/1/messages.json (token, user,
// message, optional device) via https::postForm. Owner: notify_ota.
// Policy (enforced by app, not here): only relay transitions, never repeats.
// Transport failure: evlog Err, retry ONCE after 60 s, then drop. Queue depth 1: queuing a new
// message replaces any pending one (including a pending retry).
#pragma once
#include <stdint.h>

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

}  // namespace pushover
