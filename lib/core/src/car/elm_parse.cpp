// ELM327 response parsing — port of bolt_data.py obdCmd() (PLAN §8.2). Owner: car.
#include "car/elm_parse.h"

#include <ctype.h>
#include <string.h>

namespace obd {

namespace {

constexpr size_t kMaxText = 512;
constexpr int kMaxTok = 24;

int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)toupper((unsigned char)c);
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Case-insensitive "s starts with first n chars of p" (Python str.startswith).
bool startsWith(const char* s, const char* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (!s[i] || toupper((unsigned char)s[i]) != toupper((unsigned char)p[i])) return false;
  }
  return true;
}

// Require the positive-response prefix, drop it, split the rest into byte pairs.
bool takeData(const char* part, const char* pos, ElmResponse& out) {
  out.len = 0;
  if (!startsWith(part, pos, 2)) return false;
  const char* d = part + 2;
  size_t n = strlen(d);
  for (size_t i = 0; i + 1 < n && out.len < sizeof(out.bytes); i += 2) {
    int hi = hexVal(d[i]), lo = hexVal(d[i + 1]);
    if (hi < 0 || lo < 0) { out.len = 0; return false; }
    out.bytes[out.len++] = (uint8_t)(hi * 16 + lo);
  }
  return true;
}

bool isHdr(const char* s, const char* a, const char* b, const char* c, size_t n) {
  return !strncmp(s, a, n) || !strncmp(s, b, n) || !strncmp(s, c, n);
}

}  // namespace

bool responseComplete(const char* raw, size_t len) { return memchr(raw, '>', len) != nullptr; }

ElmResponse parseResponse(const char* raw, size_t len, const char* cmd) {
  ElmResponse r;
  // 1. strip spaces, '\r' (and '\n') -> separator, drop '>' and non-printables; uppercase.
  char text[kMaxText];
  size_t n = 0;
  for (size_t i = 0; i < len && n + 1 < kMaxText; i++) {
    char c = raw[i];
    if (c == ' ' || c == '>') continue;
    if (c == '\r' || c == '\n') c = ' ';
    else if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E) continue;
    text[n++] = (char)toupper((unsigned char)c);
  }
  text[n] = 0;
  if (strstr(text, "ERROR")) { r.status = ElmStatus::CommandError; return r; }
  if (strstr(text, "NODATA")) { r.status = ElmStatus::NoData; return r; }

  size_t cl = 0;
  while (cmd[cl] && cmd[cl] != '\r') cl++;
  if (cl < 2) return r;
  int h0 = hexVal(cmd[0]), h1 = hexVal(cmd[1]);
  if (h0 < 0 || h1 < 0) return r;

  // 2. positive response prefix = first command byte + 0x40
  static const char kHex[] = "0123456789ABCDEF";
  int p = (h0 * 16 + h1 + 0x40) & 0xFF;
  const char pos[3] = {kHex[p >> 4], kHex[p & 15], 0};

  // tokenize on spaces (in place)
  char* tok[kMaxTok];
  int nt = 0;
  for (char* s = text; *s && nt < kMaxTok;) {
    while (*s == ' ') s++;
    if (!*s) break;
    tok[nt++] = s;
    while (*s && *s != ' ') s++;
    if (*s) *s++ = 0;
  }

  // 3. exact port of the Python loop
  const size_t echo_n = cl < 4 ? cl : 4;
  for (int i = 0; i < nt; i++) {
    const char* part = tok[i];
    if (startsWith(part, cmd, echo_n)) {  // echoed command
      if (i + 1 < nt) {
        const char* rp = tok[i + 1];
        if (strlen(rp) >= 4 && isHdr(rp, "7E", "7B", "7D", 2)) rp += 4;  // ECU id + length nibble
        if (!takeData(rp, pos, r)) continue;  // Python: warn + continue
        r.status = r.len ? ElmStatus::Ok : ElmStatus::Malformed;
      }
      return r;  // Python: break
    }
    if (isHdr(part, "7EC", "7EF", "7E8", 3)) {  // headers on, echo off
      const char* rp = strlen(part) >= 5 ? part + 5 : part;
      if (!takeData(rp, pos, r)) continue;
      r.status = r.len ? ElmStatus::Ok : ElmStatus::Malformed;
      return r;
    }
  }

  // Extension (echo off, headers off): a token "<pos><rest of cmd>..." e.g. "62833458".
  for (int i = 0; i < nt; i++) {
    const char* part = tok[i];
    if (!startsWith(part, pos, 2)) continue;
    if (cl > 2 && !startsWith(part + 2, cmd + 2, cl - 2)) continue;  // PID echo must match
    if (takeData(part, pos, r) && r.len) { r.status = ElmStatus::Ok; return r; }
  }
  r.len = 0;
  r.status = ElmStatus::Malformed;
  return r;
}

}  // namespace obd
