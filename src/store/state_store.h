// NVS persistence of RuntimeState (sticky state, last SOC, relay_cmd, override). Owner: store.
// save() is cheap to call often: it writes only when content changed, and SOC-only changes at most
// every 10 min (flash wear). flush() forces a write (before reboot/OTA).
#pragma once
#include "core_types.h"

namespace statestore {

bool load(RuntimeState& out);   // false => defaults. Timed overrides (until != 0) are dropped on load.
void save(const RuntimeState& s);
void flush();

}  // namespace statestore
