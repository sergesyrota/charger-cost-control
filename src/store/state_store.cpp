// STUB — owner: store.
#include "store/state_store.h"

namespace statestore {
bool load(RuntimeState& out) { out = RuntimeState(); return false; }
void save(const RuntimeState&) {}
void flush() {}
}  // namespace statestore
