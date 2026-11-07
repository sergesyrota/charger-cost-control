// ComEd feed parsers: streaming, no allocation, no regex. Owner: price.
#include "price/comed_parse.h"

#include <stdlib.h>
#include <string.h>

namespace price {

static bool isNumCh(char c) { return (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+' || c == 'e' || c == 'E'; }

// ---------------------------------------------------------------- 5-minute JSON
FiveMinParser::FiveMinParser(FivePoint* out, size_t max) : out_(out), max_(max) { reset(); }

void FiveMinParser::reset() {
  n_ = 0; bad_ = false; tok_len_ = 0; st_ = 0; key_ = 0; want_val_ = false; in_key_ = false;
  depth_ = 0; have_ = 0; pend_epoch_ = 0; pend_price_ = 0;
}

// A string or bare token just finished (tok_ holds it).
void FiveMinParser::endToken() {
  tok_[tok_len_] = 0;
  if (want_val_) {
    if (key_ == 1) {
      uint64_t v = 0;
      bool good = tok_len_ > 0;
      for (uint8_t i = 0; i < tok_len_ && good; i++) {
        if (tok_[i] >= '0' && tok_[i] <= '9') v = v * 10 + (tok_[i] - '0');
        else good = false;
      }
      if (good) { pend_epoch_ = (uint32_t)(v / 1000); have_ |= 1; }
    } else if (key_ == 2) {
      char* end = nullptr;
      double d = strtod(tok_, &end);
      if (end != tok_ && !isnan(d) && !isinf(d)) { pend_price_ = (float)d; have_ |= 2; }
    }
    want_val_ = false;
    key_ = 0;
  } else if (in_key_) {
    key_ = 0;
    if (!strcmp(tok_, "millisUTC")) key_ = 1;
    else if (!strcmp(tok_, "price")) key_ = 2;
  }
  tok_len_ = 0;
}

void FiveMinParser::feed(const char* d, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = d[i];
    if (st_ == 1) {  // inside string
      if (c == '"') { st_ = 0; endToken(); }
      else if (tok_len_ < sizeof(tok_) - 1) tok_[tok_len_++] = c;
      continue;
    }
    if (st_ == 2) {  // bare token (unquoted number)
      if (isNumCh(c)) { if (tok_len_ < sizeof(tok_) - 1) tok_[tok_len_++] = c; continue; }
      st_ = 0; endToken();  // then handle the delimiter below
    }
    switch (c) {
      case '"': st_ = 1; tok_len_ = 0; in_key_ = !want_val_; break;
      case ':': want_val_ = true; break;
      case '{': depth_++; have_ = 0; key_ = 0; want_val_ = false; break;
      case ',': want_val_ = false; key_ = 0; break;
      case '}':
        if (depth_ > 0) depth_--; else bad_ = true;
        if (have_ == 3 && n_ < max_) { out_[n_].epoch = pend_epoch_; out_[n_].price = pend_price_; n_++; }
        have_ = 0; want_val_ = false; key_ = 0;
        break;
      default:
        if (want_val_ && isNumCh(c)) { st_ = 2; tok_len_ = 0; tok_[tok_len_++] = c; }
        break;
    }
  }
}
size_t FiveMinParser::count() const { return n_; }
bool FiveMinParser::ok() const { return !bad_ && depth_ == 0; }

// ---------------------------------------------------------------- ServletFeed
static const char kLit[] = "Date.UTC(";
static const uint8_t kLitLen = sizeof(kLit) - 1;

ServletParser::ServletParser(float out[24]) : out_(out) { reset(); }

void ServletParser::reset() {
  for (int i = 0; i < 24; i++) out_[i] = NAN;
  n_ = 0; tok_len_ = 0; st_ = 0; match_ = 0; argi_ = 0; argbad_ = false; hour_ = -1;
}

void ServletParser::feed(const char* d, size_t len) {
  for (size_t i = 0; i < len; i++) {
    char c = d[i];
    switch (st_) {
      case 0:  // looking for "Date.UTC("
        if (c == kLit[match_]) {
          if (++match_ == kLitLen) { st_ = 1; match_ = 0; argi_ = 0; argbad_ = false; tok_len_ = 0; hour_ = -1; }
        } else {
          match_ = (c == kLit[0]) ? 1 : 0;
        }
        break;
      case 1:  // 6 non-negative integer args; the 4th (index 3) is the hour-ending
        if (c >= '0' && c <= '9') {
          if (tok_len_ < sizeof(tok_) - 1) tok_[tok_len_++] = c; else argbad_ = true;
        } else if (c == ',' || c == ')') {
          if (tok_len_ == 0) argbad_ = true;
          if (argi_ == 3 && tok_len_ > 0 && tok_len_ <= 2) {
            tok_[tok_len_] = 0;
            hour_ = (int8_t)atoi(tok_);
          }
          argi_++;
          tok_len_ = 0;
          if (c == ')') st_ = (argbad_ || argi_ != 6 || hour_ < 0 || hour_ > 23) ? 0 : 2;
        } else if (c != ' ') {
          argbad_ = true;
        }
        break;
      case 2:  // after ')': ',' then the number
        if (c == ',' || c == ' ') break;
        if (isNumCh(c)) { st_ = 3; tok_len_ = 0; tok_[tok_len_++] = c; }
        else st_ = 0;
        break;
      case 3:  // price number, terminated by ']'
        if (isNumCh(c)) {
          if (tok_len_ < sizeof(tok_) - 1) tok_[tok_len_++] = c; else st_ = 0;
        } else if (c == ' ') {
          // ignore
        } else if (c == ']') {
          tok_[tok_len_] = 0;
          char* end = nullptr;
          double v = strtod(tok_, &end);
          if (end != tok_ && *end == 0 && !isnan(v) && !isinf(v)) {
            if (isnan(out_[hour_])) n_++;
            out_[hour_] = (float)v;
          }
          st_ = 0;
        } else {
          st_ = 0;
        }
        break;
    }
  }
}
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
