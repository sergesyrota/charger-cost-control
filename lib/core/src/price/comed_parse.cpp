// STUB — owner: price. Replace with the real streaming tokenizers.
#include "price/comed_parse.h"

namespace price {

FiveMinParser::FiveMinParser(FivePoint* out, size_t max) : out_(out), max_(max) { reset(); }
void FiveMinParser::reset() { n_ = 0; bad_ = false; tok_len_ = 0; st_ = 0; have_ = 0; }
void FiveMinParser::feed(const char*, size_t) {}
size_t FiveMinParser::count() const { return n_; }
bool FiveMinParser::ok() const { return !bad_; }

ServletParser::ServletParser(float out[24]) : out_(out) { reset(); }
void ServletParser::reset() {
  for (int i = 0; i < 24; i++) out_[i] = NAN;
  n_ = 0; tok_len_ = 0; st_ = 0; argi_ = 0; hour_ = -1;
}
void ServletParser::feed(const char*, size_t) {}
uint8_t ServletParser::count() const { return n_; }

size_t parseFiveMin(const char* s, size_t len, FivePoint* out, size_t max) {
  FiveMinParser p(out, max);
  p.feed(s, len);
  return p.count();
}

uint8_t parseServlet(const char* s, size_t len, float out[24]) {
  ServletParser p(out);
  p.feed(s, len);
  return p.count();
}

}  // namespace price
