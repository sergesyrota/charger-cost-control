// ELM327 response parsing — exact port of bolt_data.py obdCmd() (PLAN §8.2). Pure. Owner: car.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace obd {

enum class ElmStatus : uint8_t {
  Ok,
  CommandError,  // "ERROR" in response => car asleep
  NoData,        // "NODATA" (spaces stripped) => treated as online
  Malformed,     // no positive response / can't locate data
  Timeout        // set by the transport, never by the parser
};

struct ElmResponse {
  ElmStatus status = ElmStatus::Malformed;
  uint8_t bytes[64];   // bytes[0..1] echo the PID; data starts at bytes[2] (= A)
  uint8_t len = 0;
};

// True once the accumulated raw text contains the '>' prompt.
bool responseComplete(const char* raw, size_t len);

// `raw` = all notification text accumulated for one command (echo on or off), `cmd` = command
// sent without "\r", e.g. "228334". Steps: strip spaces, '\r' -> separators, drop '>';
// ERROR / NODATA; positive prefix = first byte + 0x40; skip echo token; drop ECU-id/length prefix
// (7E/7B/7D -> 4 chars, 7E8/7EC/7EF -> 5 chars); split hex pairs.
ElmResponse parseResponse(const char* raw, size_t len, const char* cmd);

}  // namespace obd
