#pragma once
#include <cstdint>
namespace ya::power_policy {
// Radio suspension belongs to whole-device sleep, never foreground data routing.
// The persistent Wi-Fi/BLE switches are still enforced by their own services.
inline bool keep_radios_awake(bool screen_sleeping, unsigned mode, bool protected_work,
                              bool physical_wake) {
  return !screen_sleeping || mode == 0 || protected_work || physical_wake;
}
inline bool deep_due(unsigned mode, int64_t idle_ms, unsigned light_minutes, bool silent,
                     bool syncing) {
  return !syncing &&
         (mode == 2 || (mode == 3 && (silent || idle_ms >= int64_t(light_minutes) * 60000)));
}
inline int64_t restore_deadline(int64_t now_ms, uint64_t due_us, uint64_t rtc_us) {
  return now_ms + int64_t(due_us > rtc_us ? (due_us - rtc_us) / 1000 : 0);
}
} // namespace ya::power_policy
