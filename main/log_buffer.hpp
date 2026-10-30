#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ya {
constexpr unsigned LOG_CAPACITY = 100, LOG_TEXT_SIZE = 256;
constexpr int LOG_DEFAULT_LEVEL = 2; // ESP_LOG_WARN
constexpr bool log_level_valid(int level) { return level >= 0 && level <= 5; }
constexpr bool log_accepts(int threshold, int level) {
  return log_level_valid(threshold) && level > 0 && level <= threshold;
}
inline size_t log_utf8_prefix(const char *text, size_t n) {
  if (!n) return 0;
  size_t start = n - 1;
  while (start && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80) --start;
  auto lead = static_cast<unsigned char>(text[start]);
  size_t width = lead < 0x80 ? 1 : (lead & 0xe0) == 0xc0 ? 2 :
                 (lead & 0xf0) == 0xe0 ? 3 : (lead & 0xf8) == 0xf0 ? 4 : 1;
  return n - start < width ? start : n;
}
struct LogEntry {
  uint8_t level = 0;
  char text[LOG_TEXT_SIZE]{};
};
struct LogBuffer {
  uint32_t revision = 0, overwritten = 0;
  unsigned start = 0, count = 0;
  LogEntry entries[LOG_CAPACITY]{};
  const LogEntry &at(unsigned i) const { return entries[(start + i) % LOG_CAPACITY]; }
  void append(int level, const char *text) {
    if (!log_accepts(5, level) || !text || !*text) return;
    unsigned index = (start + count) % LOG_CAPACITY;
    if (count == LOG_CAPACITY) { start = (start + 1) % LOG_CAPACITY; ++overwritten; }
    else ++count;
    auto &e = entries[index];
    e.level = level;
    size_t n = std::min(strlen(text), sizeof(e.text) - 1);
    n = log_utf8_prefix(text, n);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r')) --n;
    memcpy(e.text, text, n); e.text[n] = 0;
    ++revision;
  }
  void clear() { start = count = overwritten = 0; ++revision; }
};
// The pinned ESP-IDF Log V1 supplies one fully formatted entry per call.
inline int log_line_level(const char *text) {
  if (!text) return 0;
  if (text[0] == '\033' && text[1] == '[') {
    text += 2;
    while ((*text >= '0' && *text <= '9') || *text == ';') ++text;
    if (*text != 'm') return 0;
    ++text;
  }
  constexpr const char *levels = "EWIDV";
  const char *p = strchr(levels, *text);
  return *text && p && text[1] == ' ' ? int(p - levels) + 1 : 0;
}
// Remove terminal escapes and control bytes. Never leave a partial UTF-8 tail.
inline void log_clean(char *text, size_t capacity, size_t formatted_size) {
  if (!capacity) return;
  size_t n = std::min(formatted_size, capacity - 1);
  n = log_utf8_prefix(text, n);
  size_t out = 0;
  for (size_t i = 0; i < n; ++i) {
    if (text[i] == '\033' && i + 1 < n && text[i + 1] == '[') {
      i += 2;
      while (i < n && (text[i] < '@' || text[i] > '~')) ++i;
    } else if (static_cast<unsigned char>(text[i]) >= 32 || text[i] == '\n')
      text[out++] = text[i];
  }
  text[out] = 0;
}
} // namespace ya
