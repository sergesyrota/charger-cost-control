// STUB — owner: notify_ota.
#include "notify/pushover.h"

namespace pushover {
void configure(const Creds&) {}
bool ready() { return false; }
bool queue(const char*) { return false; }
bool sendNow(const char*, char* err, uint16_t cap) { if (err && cap) err[0] = 0; return false; }
void loop() {}
}  // namespace pushover
