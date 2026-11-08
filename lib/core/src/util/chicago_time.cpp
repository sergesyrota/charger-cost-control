#include "util/chicago_time.h"

// days since 1970-01-01 -> civil date (Howard Hinnant's algorithm)
static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t yy = (int64_t)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = (int)(yy + (m <= 2));
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

static int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

// UTC epoch of local 02:00 on the n-th Sunday (n>=1) of month m, given the local offset before the switch.
static int64_t nthSunday2am(int y, unsigned m, int n, int32_t offset_before) {
  int64_t first = daysFromCivil(y, m, 1);
  int wdayFirst = (int)((first % 7 + 7 + 4) % 7);  // 1970-01-01 was Thursday (4)
  int64_t day = first + (7 - wdayFirst) % 7 + 7 * (n - 1);
  return day * 86400 + 2 * 3600 - offset_before;
}

int32_t chicagoOffset(int64_t epoch) {
  int y; unsigned m, d;
  civilFromDays(floorDiv(epoch - 6 * 3600, 86400), y, m, d);
  int64_t start = nthSunday2am(y, 3, 2, -6 * 3600);
  int64_t end = nthSunday2am(y, 11, 1, -5 * 3600);
  return (epoch >= start && epoch < end) ? -5 * 3600 : -6 * 3600;
}

ChicagoTime chicagoFromEpoch(int64_t epoch) {
  ChicagoTime t;
  t.epoch = epoch;
  t.utc_offset_s = chicagoOffset(epoch);
  int64_t local = epoch + t.utc_offset_s;
  int64_t days = floorDiv(local, 86400);
  int64_t sod = local - days * 86400;
  int y; unsigned m, d;
  civilFromDays(days, y, m, d);
  t.year = (int16_t)y; t.month = (uint8_t)m; t.day = (uint8_t)d;
  t.hour = (uint8_t)(sod / 3600); t.minute = (uint8_t)(sod % 3600 / 60); t.second = (uint8_t)(sod % 60);
  t.wday = (uint8_t)((days % 7 + 7 + 4) % 7);
  return t;
}

uint32_t addDays(uint32_t ymd, int n) {
  int64_t days = daysFromCivil((int)(ymd / 10000), (ymd / 100) % 100, ymd % 100) + n;
  int y; unsigned m, d;
  civilFromDays(days, y, m, d);
  return (uint32_t)y * 10000u + m * 100u + d;
}
