#include "identity.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "ya.hpp"
#include "power.hpp"
#include "logs.hpp"
namespace ya {
TaskHandle_t main_task_handle = nullptr;
}
extern "C" const ya::FirmwareIdentity ya_firmware_identity
    __attribute__((used, section(".rodata_custom_desc"))) = {
        ya::IDENTITY_MAGIC, "esp32-s3-touch-amoled-2.16", "ab6-v1"};
extern "C" void app_main() {
  ya::logs_init();
  heap_caps_register_failed_alloc_callback([](size_t n, uint32_t caps, const char *fn) {
    esp_rom_printf("YA_ALLOC_FAIL bytes=%u caps=0x%x free=%u largest=%u fn=%s\n", unsigned(n),
                   unsigned(caps), unsigned(heap_caps_get_free_size(caps)),
                   unsigned(heap_caps_get_largest_free_block(caps)), fn);
  });
  ya::main_task_handle = xTaskGetCurrentTaskHandle();
  // Cover initialization deadlocks as well as failures after the desktop is running.
  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
  auto e = ya::config_init();
  if (e != ESP_OK) {
    ESP_LOGE("boot", "Configuration init failed: %s (NVS retained)", esp_err_to_name(e));
    esp_ota_mark_app_invalid_rollback_and_reboot();
    abort();
  }
  ya::logs_load_level();
  esp_ota_img_states_t boot_state;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &boot_state) == ESP_OK) {
    ya::Guard g(ya::state_mutex);
    ya::state.pending = boot_state == ESP_OTA_IMG_PENDING_VERIFY;
  }
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ya::power_init();
  ESP_ERROR_CHECK(ya::board_init());
  ya::services_init();
  ya::ui_start();
  ya::core_ready = true;
  ya::diagnostics_init();
  ya::ota_boot_check();
  int64_t report = 0;
  for (;;) {
    auto t = ya::now_ms();
    ya::power_tick();
    if (!ya::power_paused && t - ya::ui_heartbeat.load() < 5000 &&
        t - ya::service_heartbeat.load() < 10000)
      esp_task_wdt_reset();
    if (t - report >= 30000) {
      report = t;
      // A full Status here reserves several KiB for the whole app_main frame,
      // including initialization and OTA verification nested below it.
      int battery;
      bool valid, wifi, sd;
      {
        ya::Guard g(ya::state_mutex);
        battery = ya::state.battery.percent;
        valid = ya::state.battery.valid;
        wifi = ya::state.wifi_connected;
        sd = ya::state.sd_mounted;
      }
      ESP_LOGI("health",
               "uptime=%lld heap=%lu battery=%d%% valid=%d wifi=%d sd=%d "
               "ui_age=%lld",
               t / 1000, (unsigned long)esp_get_free_heap_size(), battery, valid, wifi, sd,
               t - ya::ui_heartbeat.load());
    }
    // A short press remains latched and wakes this task immediately, even if
    // the key has already been released before the next periodic power tick.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
  }
}
