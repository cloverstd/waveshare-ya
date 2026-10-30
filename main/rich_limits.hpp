#pragma once
#include <cstddef>
#include <cstdint>
#include "zlib.h"
namespace ya::rich {
constexpr size_t WIDTH = 432, MAX_HEIGHT = 2048, MAX_PACKED = 128 * 1024;
inline bool dimensions(size_t height, size_t text, size_t packed) {
  return height > 0 && height <= MAX_HEIGHT && text > 0 && text <= 4000 &&
         packed > 0 && packed <= MAX_PACKED;
}
inline bool text_valid(const uint8_t *s, size_t size) {
  size_t count = 0;
  for (size_t i = 0; i < size;) {
    uint32_t c = s[i++], cp = c, minimum = 0;
    unsigned more = 0;
    if (!c) return false;
    if (c >= 0xc2 && c <= 0xdf) { more = 1; cp = c & 31; minimum = 128; }
    else if (c >= 0xe0 && c <= 0xef) { more = 2; cp = c & 15; minimum = 2048; }
    else if (c >= 0xf0 && c <= 0xf4) { more = 3; cp = c & 7; minimum = 65536; }
    else if (c >= 128) return false;
    while (more--) {
      if (i == size || (s[i] & 0xc0) != 0x80) return false;
      cp = (cp << 6) | (s[i++] & 63);
    }
    if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) || ++count > 1000) return false;
  }
  return count > 0;
}
inline bool unpack(const uint8_t *packed, size_t packed_size, size_t height, size_t text_size,
                   uint8_t *out, size_t capacity) {
  if (!dimensions(height, text_size, packed_size)) return false;
  const size_t expected = text_size + WIDTH * height * 2;
  if (capacity != expected) return false;
  uLongf n = capacity;
  return uncompress(out, &n, packed, packed_size) == Z_OK && n == expected && text_valid(out, text_size);
}

}
