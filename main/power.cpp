#include "notifications.hpp"
#include "companion.hpp"
#include "power.hpp"
#include "power_policy.hpp"
#include "wake_handoff.hpp"
#include "data.hpp"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lv_adapter.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_rtc_time.h"
#include "esp_task_wdt.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <algorithm>
namespace ya {
std::atomic<bool> screen_sleeping{false}, power_paused{false}, power_wake_ui{false};
namespace {
PowerPrefs prefs;
struct SleepDelay { uint16_t battery = 10, usb = 10; } delays;
SemaphoreHandle_t mutex;
esp_pm_lock_handle_t awake_lock = nullptr;
std::atomic<int64_t> notify_until{0}, last_notify{-60000}, hold_until{0};
power_wake::Handoff wake_handoff;
bool silent = false;
std::atomic<bool> paused{false}, syncing{false};
int64_t off_at = 0, budget = 0;
std::atomic<int64_t> sync_at{0};
struct Retained {
  uint32_t magic;
  uint64_t due;
  uint32_t check;
};
RTC_DATA_ATTR Retained retained{};
uint32_t checksum(uint64_t due) { return 0x59415033u ^ uint32_t(due) ^ uint32_t(due >> 32); }
bool held() {
  return gpio_get_level(GPIO_NUM_16) != 0 || gpio_get_level(GPIO_NUM_0) == 0 ||
         gpio_get_level(GPIO_NUM_18) == 0;
}
void resume() {
  if (!paused)
    return;
  esp_pm_lock_acquire(awake_lock);
  bsp_display_sleep_set(false);
  // Publish the logical wake before allowing UI input timers to run again.
  power_wake_ui = true;
  screen_sleeping = false;
  esp_lv_adapter_resume();
  paused = false;
  power_paused = false;
  silent = false;
  off_at = 0;
  syncing = false;
  wifi_runtime(true);
  ble_runtime(true);
  esp_task_wdt_add(nullptr);
}
bool suspend() {
  if (paused)
    return true;
  if (esp_lv_adapter_pause(1000) != ESP_OK)
    return false;
  if (bsp_display_sleep_set(true) != ESP_OK) {
    esp_lv_adapter_resume();
    return false;
  }
  paused = true;
  power_paused = true;
  esp_task_wdt_delete(nullptr);
  esp_pm_lock_release(awake_lock);
  return true;
}
void deep() {
  if (wake_handoff.pending())
    return;
  if (companion_busy() || data_busy() || !data_checkpoint() || !notifications_checkpoint())
    return;
  bool pending;
  {
    Guard g(state_mutex);
    pending = state.pending;
  }
  if (ota_busy || pending || held())
    return;
  // Storage leases are drained before sleeping; failed unmount aborts the attempt.
  bool mounted;
  {
    Guard g(state_mutex);
    mounted = state.sd_mounted;
  }
  if (mounted) {
    storage_unmount();
    Guard g(state_mutex);
    if (state.sd_mounted)
      return;
  }
  // Any later return aborts preparation and restores a card we unmounted.
  // Declared before mutation so its lock is released before remounting.
  power_policy::RestoreStorageOnExit restore_storage(mounted, storage_mount);
  if (!suspend())
    return;
  wifi_runtime(false);
  ble_runtime(false);
  Guard mutation(mutation_mutex);
  if (ota_busy)
    return;
  retained.magic = 0x59415033;
  uint64_t wait = sync_at > now_ms() ? uint64_t(sync_at - now_ms()) * 1000 : 1000000;
  retained.due =
      data_has_plans() && power_profile().sync_minutes ? esp_rtc_get_time_us() + wait : 0;
  retained.check = checksum(retained.due);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  // RTC GPIO 16 is active high, BOOT 0 and HOME 18 active low.
  if (esp_sleep_enable_ext0_wakeup(GPIO_NUM_16, 1) != ESP_OK ||
      esp_sleep_enable_ext1_wakeup_io((1ULL << 0) | (1ULL << 18), ESP_EXT1_WAKEUP_ANY_LOW) !=
          ESP_OK) {
    ESP_LOGE("power", "Cannot configure button wake sources; sleep aborted");
    return;
  }
  if (retained.due && esp_sleep_enable_timer_wakeup(wait) != ESP_OK)
    return;
  // Do not use PMU shutdown; charging and RTC supplies stay on.
  vTaskDelay(pdMS_TO_TICKS(100));
  if (!notifications_checkpoint()) return;
  if (wake_handoff.pending() || held() || !screen_sleeping)
    return;
  esp_deep_sleep_start();
}
} // namespace
void power_init() {
  mutex = xSemaphoreCreateMutex();
  if (!mutex)
    abort();
  size_t n = sizeof prefs;
  PowerPrefs loaded;
  if (app_config_get("sys_power", 1, &loaded, &n) == ESP_OK && n == sizeof loaded) {
    auto valid = [](const PowerProfile &p) {
      return p.mode <= 3 && p.ble_receive <= 1 && p.route <= 2 && p.light_minutes >= 1 &&
             p.light_minutes <= 1440 && p.sync_minutes <= 1440;
    };
    if (valid(loaded.battery) && valid(loaded.usb))
      prefs = loaded;
  }
  size_t delay_size = sizeof delays;
  SleepDelay loaded_delay;
  if (app_config_get("sys_delay", 1, &loaded_delay, &delay_size) == ESP_OK &&
      delay_size == sizeof loaded_delay && loaded_delay.battery <= 1440 && loaded_delay.usb <= 1440)
    delays = loaded_delay;
  if (retained.magic == 0x59415033 && retained.check == checksum(retained.due) && retained.due) {
    auto rtc = esp_rtc_get_time_us();
    sync_at = power_policy::restore_deadline(now_ms(), retained.due, rtc);
  }
  silent = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER && retained.magic == 0x59415033 &&
           retained.check == checksum(retained.due);
  if (silent) {
    screen_sleeping = true;
    off_at = 1;
    sync_at = now_ms();
  }
  ESP_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "ya_awake", &awake_lock));
  ESP_ERROR_CHECK(esp_pm_lock_acquire(awake_lock));
  esp_pm_config_t pm{};
  pm.max_freq_mhz = 240;
  pm.min_freq_mhz = 80;
  pm.light_sleep_enable = true;
  ESP_ERROR_CHECK(esp_pm_configure(&pm));
  for (auto pin : {GPIO_NUM_0, GPIO_NUM_18})
    gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable(GPIO_NUM_16, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();
}
bool power_silent_boot() { return silent; }
void power_request_wake() {
  wake_handoff.request([] {
    if (main_task_handle)
      xTaskNotifyGive(main_task_handle);
  });
}
PowerProfile power_profile() {
  bool usb;
  {
    Guard g(state_mutex);
    usb = state.battery.valid && state.battery.usb;
  }
  Guard g(mutex);
  return usb ? prefs.usb : prefs.battery;
}
int power_value(const char *k) {
  Guard g(mutex);
  auto p = strstr(k, "usb") ? prefs.usb : prefs.battery;
  if (strstr(k, "delay")) return strstr(k, "usb") ? delays.usb : delays.battery;
  if (strstr(k, "mode"))
    return p.mode;
  if (strstr(k, "light"))
    return p.light_minutes;
  if (strstr(k, "sync"))
    return p.sync_minutes;
  if (strstr(k, "ble"))
    return p.ble_receive;
  return p.route;
}
esp_err_t power_setting(const char *k, int value) {
  if (ota_busy)
    return ESP_ERR_INVALID_STATE;
  if (!strcmp(k, "power_battery_delay") || !strcmp(k, "power_usb_delay")) {
    if (value < 0 || value > 1440) return ESP_ERR_INVALID_ARG;
    SleepDelay next;
    { Guard g(mutex); next = delays; }
    (strstr(k, "usb") ? next.usb : next.battery) = value;
    auto e = app_config_set("sys_delay", 1, &next, sizeof next);
    if (e == ESP_OK) { Guard g(mutex); delays = next; }
    return e;
  }
  PowerPrefs next;
  {
    Guard g(mutex);
    next = prefs;
  }
  auto &p = strstr(k, "usb") ? next.usb : next.battery;
  if (!strcmp(k, "power_battery_mode") || !strcmp(k, "power_usb_mode")) {
    if (value < 0 || value > 3)
      return ESP_ERR_INVALID_ARG;
    p.mode = value;
  } else if (!strcmp(k, "power_battery_light") || !strcmp(k, "power_usb_light")) {
    if (value < 1 || value > 1440)
      return ESP_ERR_INVALID_ARG;
    p.light_minutes = value;
  } else if (!strcmp(k, "power_battery_sync") || !strcmp(k, "power_usb_sync")) {
    if (value < 0 || value > 1440)
      return ESP_ERR_INVALID_ARG;
    p.sync_minutes = value;
  } else if (!strcmp(k, "power_battery_ble") || !strcmp(k, "power_usb_ble")) {
    if (value < 0 || value > 1)
      return ESP_ERR_INVALID_ARG;
    p.ble_receive = value;
  } else if (!strcmp(k, "power_battery_route") || !strcmp(k, "power_usb_route")) {
    if (value < 0 || value > 2)
      return ESP_ERR_INVALID_ARG;
    p.route = value;
  } else
    return ESP_ERR_INVALID_ARG;
  auto e = app_config_set("sys_power", 1, &next, sizeof next);
  if (e == ESP_OK) {
    Guard g(mutex);
    prefs = next;
    sync_at = 0;
  }
  return e;
}
std::string power_diagnostics() {
  return std::string(paused ? "light" : screen_sleeping ? "screen-off" : "active") + (syncing ? " syncing" : "") +
         " wake=" + std::to_string(esp_sleep_get_wakeup_cause());
}
void power_notify() {
  auto now = now_ms();
  if (now - last_notify.load() >= 60000) {
    last_notify = now;
    notify_until = now + 10000;
  }
}
bool power_notification_active() { return now_ms() < notify_until.load(); }
void power_hold(unsigned ms) { hold_until = now_ms() + std::min(ms, 120000u); }
void power_tick() {
  const bool physical_wake = wake_handoff.publish(screen_sleeping, power_wake_ui);
  if (physical_wake) {
    silent = false;
    off_at = 0;
    syncing = false;
  }
  bool pending;
  {
    Guard g(state_mutex);
    pending = state.pending;
  }
  auto p = power_profile();
  if (power_policy::keep_radios_awake(screen_sleeping, p.mode, ota_busy || pending,
                                     physical_wake)) {
    // Apply even when resume() is a no-op: a notification or power-source change
    // may have resumed the display while leaving the radios suspended.
    wifi_runtime(true);
    ble_runtime(true);
    resume();
    if (!screen_sleeping)
      off_at = 0;
    return;
  }
  auto now = now_ms();
  if (!off_at)
    off_at = now;
  if (!sync_at)
    sync_at = now + int64_t(p.sync_minutes) * 60000;
  if (power_notification_active()) {
    if (paused) {
      esp_pm_lock_acquire(awake_lock);
      bsp_display_sleep_set(false);
      esp_lv_adapter_resume();
      paused = false;
      power_paused = false;
      esp_task_wdt_add(nullptr);
    }
    return;
  }
  const bool usb = [] { Guard g(state_mutex); return state.battery.valid && state.battery.usb; }();
  const int64_t delay_ms = silent ? 0 : int64_t(power_value(usb ? "power_usb_delay" : "power_battery_delay")) * 60000;
  if (now < hold_until || companion_busy() || power_policy::screen_only_window(now - off_at, delay_ms / 60000, silent)) {
    wifi_runtime(true); ble_runtime(true);
    return;
  }
  if (!suspend())
    return;
  bool due = data_has_plans() && p.sync_minutes && now >= sync_at;
  if (due && !syncing) {
    syncing = true;
    budget = now + 60000;
    data_sync();
    sync_at = now + int64_t(p.sync_minutes) * 60000;
  }
  if (syncing) {
    ble_runtime(true);
    wifi_runtime(p.route == 2 || now >= budget - 40000);
    // A queue hand-off needs time before data_busy reflects the new round.
    if (now >= budget) {
      data_cancel_round();
      syncing = false;
    } else if (now > budget - 57000 && !data_busy())
      syncing = false;
  } else {
    wifi_runtime(false);
    ble_runtime(p.ble_receive);
  }
  if (power_policy::deep_due(p.mode, now - off_at - delay_ms, p.light_minutes, silent, syncing))
    deep();
}
} // namespace ya
