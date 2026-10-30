#pragma once
#include <cstdint>
namespace ya::ui_policy {
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
