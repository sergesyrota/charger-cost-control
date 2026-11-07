// HTTPS OTA (PLAN §12). Manifest {"version","url","sha256","size","notes"} or a direct .bin URL
// (no sha256 check). https:// only. Streams to the inactive slot with esp_ota_*, verifies size +
// sha256, sets boot partition. Does NOT reboot (app flushes + reboots). Rollback: verifyRollbackLater()
// returns true (defined in updater.cpp); markValid() after health checks. Owner: notify_ota.
// check()/install() are blocking: loop task only. progress()/info getters are thread-safe.
#pragma once
#include <stdint.h>

namespace ota {

enum class Phase : uint8_t { Idle, Checking, Available, UpToDate, Downloading, Verifying, Done, Error };
const char* phaseName(Phase p);  // "idle" "checking" "available" "up_to_date" "downloading" "verifying" "done" "error"

struct Manifest {
  char version[24] = "";
  char url[256] = "";
  char sha256[65] = "";    // "" for direct .bin
  uint32_t size = 0;       // 0 unknown
  char notes[200] = "";
  bool direct_bin = false;
};

struct Progress {
  Phase phase = Phase::Idle;
  uint32_t bytes = 0, total = 0;
  char error[80] = "";
  bool has_manifest = false;
  Manifest manifest;       // last checked
};

bool check(const char* url);          // sets phase Available / UpToDate / Error
bool install(const char* url);        // url "" => use last checked manifest; sets Done on success
Progress progress();

bool runningFromRollback();           // booted the previous slot after a failed update
bool pendingVerify();                 // current image is PENDING_VERIFY (not yet marked valid)
void markValid();                     // esp_ota_mark_app_valid_cancel_rollback()

}  // namespace ota
