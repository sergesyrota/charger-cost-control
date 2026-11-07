// Minimal HTTPS client: CA bundle validation, follows redirects (GitHub release assets), streams the
// body to a callback. Rejects non-https URLs. Takes netlock internally for the whole request.
// Blocking — call from the loop task only. Owner: net.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <functional>

namespace https {

// Return false from the callback to abort (result.error = "aborted").
using BodyCb = std::function<bool(const uint8_t* data, size_t len)>;

struct Result {
  int status = 0;              // final HTTP status (after redirects); <0 = transport error
  int32_t content_length = -1; // -1 unknown / chunked
  size_t bytes = 0;            // body bytes delivered to the callback
  const char* error = "";      // static string, "" on success
  bool ok() const { return status >= 200 && status < 300 && !*error; }
};

// `on_start` (optional) is called once with the final status + content length before body bytes.
using StartCb = std::function<void(int status, int32_t content_length)>;

Result get(const char* url, const BodyCb& body, uint32_t timeout_ms = 15000, const StartCb& on_start = nullptr);

// application/x-www-form-urlencoded POST (Pushover). `form` must already be url-encoded.
Result postForm(const char* url, const char* form, const BodyCb& body, uint32_t timeout_ms = 15000);

// Percent-encode a UTF-8 value for form bodies.
void urlEncodeAppend(const char* value, std::function<void(const char*, size_t)> sink);

}  // namespace https
