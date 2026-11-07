// STUB — owner: net.
#include "net/net_lock.h"

namespace netlock {
void init() {}
bool take(const char*, uint32_t) { return true; }
void give() {}
const char* holder() { return ""; }
}  // namespace netlock
