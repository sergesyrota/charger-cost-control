// UTF-8 helpers for fixed char buffers (user-facing text is Russian: 2 bytes per letter). Pure.
#pragma once
#include <stddef.h>
#include <string.h>

// Drops an incomplete trailing UTF-8 sequence, e.g. one cut by snprintf/strlcpy truncation, so a
// truncated buffer never ends in half a character. Returns the new length.
inline size_t utf8FixTail(char* s) {
  if (!s) return 0;
  size_t n = strlen(s);
  if (!n) return 0;
  size_t j = n - 1;
  int cont = 0;
  while (j > 0 && cont < 3 && ((unsigned char)s[j] & 0xC0) == 0x80) { j--; cont++; }
  const unsigned char lead = (unsigned char)s[j];
  size_t need = lead < 0x80 ? 1 : (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
  if (lead >= 0x80 && n - j < need) { s[j] = 0; return j; }
  return n;
}
