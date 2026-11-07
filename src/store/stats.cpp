// STUB — owner: store.
#include "store/stats.h"

namespace stats {
void begin() {}
void sample(const ChicagoTime*, const PriceSnapshot&, const CarSnapshot&, bool, Mode) {}
void flush() {}
bool currentSession(rec::Session&) { return false; }
bool lastSession(rec::Session&) { return false; }
size_t fillHourly(uint32_t, uint32_t, uint32_t&, char*, size_t) { return 0; }
size_t fillDetail(uint32_t, uint32_t, uint32_t&, char*, size_t) { return 0; }
size_t fillSessions(uint16_t, uint32_t&, char*, size_t) { return 0; }
size_t daily(uint16_t, Agg*, size_t) { return 0; }
size_t monthly(uint16_t, Agg*, size_t) { return 0; }
FsInfo fsInfo() { return FsInfo{0, 0}; }
}  // namespace stats
