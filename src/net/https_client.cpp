// STUB — owner: net.
#include "net/https_client.h"

namespace https {
Result get(const char*, const BodyCb&, uint32_t, const StartCb&) { Result r; r.status = -1; r.error = "not implemented"; return r; }
Result postForm(const char*, const char*, const BodyCb&, uint32_t) { Result r; r.status = -1; r.error = "not implemented"; return r; }
void urlEncodeAppend(const char*, std::function<void(const char*, size_t)>) {}
}  // namespace https
