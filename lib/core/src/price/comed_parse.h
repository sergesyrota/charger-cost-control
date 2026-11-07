// ComEd feed parsers. Pure, push-style (fed in chunks as bytes arrive) so HTTP bodies are never
// buffered whole. Port of comed-realtime-price-dashboard/price-worker/src/comed.ts.
// Owner: price.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "core_types.h"

namespace price {

// 5-minute feed: JSON array, newest first, items {"millisUTC":"1794087900000","price":"16.7"}
// (both values are strings). Writes points in feed order into `out` (up to `max`, extra ignored).
class FiveMinParser {
 public:
  FiveMinParser(FivePoint* out, size_t max);
  void reset();
  void feed(const char* data, size_t len);  // any chunk size, including 1 byte
  size_t count() const;                     // points parsed so far
  bool ok() const;                          // false if structure was clearly malformed

 private:
  FivePoint* out_;
  size_t max_, n_ = 0;
  bool bad_ = false;
  // tokenizer state (implementation-defined)
  char tok_[24];
  uint8_t tok_len_ = 0;
  uint8_t st_ = 0;
  uint32_t pend_epoch_ = 0;
  float pend_price_ = 0;
  uint8_t have_ = 0;
};

// ServletFeed (day-ahead "daynexttoday" and actuals "day"): JS text
// `[[Date.UTC(2026,10,7,0,0,0), 3.5], ...]`. The 4th Date.UTC arg is the hour-ending index 0..23
// (index 0 = 11 pm -> midnight). Month/day/year args are ignored. out[i] = NAN where missing.
class ServletParser {
 public:
  explicit ServletParser(float out[24]);   // fills out with NAN
  void reset();
  void feed(const char* data, size_t len);
  uint8_t count() const;                   // number of hours filled

 private:
  float* out_;
  uint8_t n_ = 0;
  char tok_[24];
  uint8_t tok_len_ = 0;
  uint8_t st_ = 0;
  uint8_t argi_ = 0;
  int8_t hour_ = -1;
};

// Whole-buffer conveniences (tests / small inputs).
size_t parseFiveMin(const char* s, size_t len, FivePoint* out, size_t max);
uint8_t parseServlet(const char* s, size_t len, float out[24]);

}  // namespace price
