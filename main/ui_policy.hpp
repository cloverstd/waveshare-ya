#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
namespace ya::ui_policy {
inline void url_base(char *out, size_t capacity, const char *url) {
  if (!capacity) return;
  const char *authority = std::strstr(url, "://");
  if (!authority) { out[0] = 0; return; }
  const size_t end = (authority + 3 - url) + std::strcspn(authority + 3, "/?#");
  std::snprintf(out, capacity, "%.*s", int(end), url);
}
inline void format_bytes(char *out, size_t capacity, uint64_t bytes) {
  static constexpr const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
  double value = bytes;
  unsigned unit = 0;
  while (value >= 1024 && unit < 6) {
    value /= 1024;
    ++unit;
  }
  if (unit)
    std::snprintf(out, capacity, "%.2f %s", value, units[unit]);
  else
    std::snprintf(out, capacity, "%llu B", (unsigned long long)bytes);
}
// Registry app roots return home; pages opened beneath Settings return there.
// Diagnostic page numbers do not encode the navigation hierarchy.
constexpr int back_page(int page, bool app_active) {
  return page != 0 && !app_active ? 1 : 0;
}
constexpr uint32_t battery_color(bool valid, int percent) {
  if (!valid || percent < 0 || percent > 100)
    return 0x596779;
  if (percent < 10)
    return 0xff7575;
  if (percent <= 50)
    return 0xffd166;
  return 0x71d7a4;
}
} // namespace ya::ui_policy
