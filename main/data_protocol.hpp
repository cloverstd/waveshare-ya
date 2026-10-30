#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <algorithm>
namespace ya::protocol {
constexpr size_t LIMIT = 4096;
// Web Bluetooth may take ~100 ms per acknowledged 20-byte write. Its ACK
// cannot interrupt another frame, and providers get an 8-second I/O budget.
constexpr int64_t ACK_TIMEOUT_MS = 25000;
constexpr int64_t RESPONSE_TIMEOUT_MS = 35000;
constexpr int64_t REQUEST_TIMEOUT_MS = 45000;
// Physical connection includes the two-minute user pairing flow. Once hello
// arrives, the authenticated data handshake has its own short bounded budget.
constexpr bool handshake_expired(bool hello_received, int64_t elapsed_ms) {
  return elapsed_ms > (hello_received ? 30000 : 120000);
}
// A frame belongs to one connection. Never reuse its buffer across reconnects.
struct Frame {
  std::string bytes;
  uint32_t wanted = 0;
  bool feed(const uint8_t *p, size_t n, std::string &out) {
    out.clear();
    if (bytes.size() + n > LIMIT + 4) {
      reset();
      return false;
    }
    bytes.append(reinterpret_cast<const char *>(p), n);
    if (bytes.size() >= 4 && !wanted) {
      wanted = (uint32_t(uint8_t(bytes[0])) << 24) | (uint32_t(uint8_t(bytes[1])) << 16) |
               (uint32_t(uint8_t(bytes[2])) << 8) | uint8_t(bytes[3]);
      if (!wanted || wanted > LIMIT) {
        reset();
        return false;
      }
    }
    if (wanted && bytes.size() >= wanted + 4) {
      if (bytes.size() != wanted + 4) {
        reset();
        return false;
      }
      out.assign(bytes.data() + 4, wanted);
      reset();
    }
    return true;
  }
  void reset() {
    bytes.clear();
    wanted = 0;
  }
};
inline std::string frame(const std::string &s) {
  uint32_t n = s.size();
  char h[] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  return std::string(h, 4) + s;
}
inline bool uuid(const char *s) {
  if (!s || strlen(s) != 36)
    return false;
  for (int i = 0; i < 36; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (s[i] != '-')
        return false;
    } else if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') ||
                 (s[i] >= 'A' && s[i] <= 'F')))
      return false;
  }
  return true;
}
inline bool bounded_json(const std::string &s) {
  if (s.empty() || s.size() > LIMIT || s.find('\0') != std::string::npos)
    return false;
  // Validate Unicode before cJSON (which does not validate raw UTF-8).
  for (size_t i = 0; i < s.size();) {
    uint8_t c = s[i++];
    if (c < 128)
      continue;
    unsigned n;
    uint32_t cp, min;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      cp = c & 31;
      min = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      cp = c & 15;
      min = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      cp = c & 7;
      min = 65536;
    } else
      return false;
    if (i + n > s.size())
      return false;
    while (n--) {
      uint8_t b = s[i++];
      if ((b & 0xc0) != 0x80)
        return false;
      cp = (cp << 6) | (b & 63);
    }
    if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
      return false;
  }
  int depth = 0;
  bool quote = false, escape = false;
  for (char c : s) {
    if (quote) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == '"')
        quote = false;
    } else if (c == '"')
      quote = true;
    else if (c == '{' || c == '[') {
      if (++depth > 6)
        return false;
    } else if (c == '}' || c == ']') {
      if (--depth < 0)
        return false;
    }
  }
  return !quote && depth == 0;
}
inline bool better(int active, int candidate, bool healthy, bool cooled) {
  return !healthy || (cooled && candidate >= active + 8);
}
} // namespace ya::protocol
