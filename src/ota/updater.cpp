// STUB — owner: notify_ota.
#include "ota/updater.h"

namespace ota {
const char* phaseName(Phase p) {
  static const char* const n[] = {"idle", "checking", "available", "up_to_date", "downloading", "verifying", "done", "error"};
  return n[(int)p];
}
bool check(const char*) { return false; }
bool install(const char*) { return false; }
Progress progress() { return Progress(); }
bool runningFromRollback() { return false; }
bool pendingVerify() { return false; }
void markValid() {}
}  // namespace ota
