#pragma once
#include "esp_err.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "wifi.hpp"
namespace ya {
constexpr const char *PRODUCT = "waveshare-ya", *BOARD = "esp32-s3-touch-amoled-2.16",
                     *LAYOUT = "ab6-v1";
constexpr const char *UPDATE_BASE_URL = "https://ya.cloverstd.com";
constexpr const char *UPDATE_MANIFEST_URL = "https://ya.cloverstd.com/api/v1/ota/manifest.json";
constexpr size_t OTA_MAX = 0x600000;
struct Config {
  uint32_t schema = 1;
  uint8_t brightness = 60, volume = 30, fps = 30;
  bool mute = false, wifi = true, ble = false;
  uint16_t sleep_seconds = 60;
  char ssid[33]{}, password[65]{}, update_url[384]{};
};
struct SystemPrefs {
  uint16_t sleep_battery = 30, sleep_usb = 300;
};
enum class OtaPhase {
  IDLE,
  CHECKING,
  AVAILABLE,
  CURRENT,
  TRANSFERRING,
  VERIFYING,
  REBOOTING,
  BOOT_VERIFY,
  COMPLETE,
  FAILED,
  ROLLED_BACK
};
const char *ota_phase_name(OtaPhase phase);
struct SettingResult {
  uint32_t id;
  esp_err_t error;
};
struct Battery {
  bool valid = false, present = false, usb = false, charging = false, discharging = false,
       full = false;
  int percent = -1, millivolts = 0;
  float temperature = 0;
  int64_t sampled = 0;
};
struct Peer {
  char name[48]{}, address[32]{};
  int rssi = 0, handle = -1, type = 0;
  bool connected = false, bonded = false;
};
struct BleLink {
  int handle = -1;
  bool encrypted = false, authenticated = false, subscribed = false, ready = false;
  char address[24]{};
  int disconnect_reason = 0;
};
struct Status {
  WifiState wlan;
  Battery battery;
  char wifi_ssid[33]{}, time_note[96]{};
  int wifi_rssi = 0;
  bool sd_capacity_valid = false, flash_capacity_valid = false;
  bool time_valid = false, time_syncing = false, rtc_valid = false, flash_mounted = false;
  int64_t last_time_sync = 0;
  uint32_t ram_free = 0, ram_total = 0, psram_free = 0, psram_total = 0;
  uint32_t internal_min = 0, internal_largest = 0;
  uint64_t flash_total = 0, flash_free = 0;
  OtaPhase ota_phase = OtaPhase::IDLE;
  uint32_t ota_received = 0, ota_total = 0;
  int64_t notice_expires = 0;
  bool wifi_connected = false, wifi_busy = false, sd_mounted = false, ble_ready = false,
       pending = false;
  char ip[24]{}, wifi_note[96]{}, notice[160]{}, ota_note[160]{}, ota_version[32]{}, ota_url[256]{},
      ble_note[128]{};
  char aps[16][33]{};
  int ap_count = 0;
  Peer peers[16]{};
  char bonds[6][32]{};
  int bond_count = 0;
  bool phone_connected = false;
  BleLink gateways[2]{};
  int64_t pair_until = 0;
  bool pair_code_valid = false, ble_scanning = false, ble_services_valid = false;
  int ble_disconnect_reason = 0;
  char ble_drop_note[48]{};
  int peer_count = 0;
  uint32_t ble_scan_version = 0;
  uint32_t pair_code = 0;
  uint64_t sd_total = 0, sd_free = 0;
  int ota_progress = 0;
  bool update_available = false;
};
extern SemaphoreHandle_t state_mutex, config_mutex, mutation_mutex;
extern std::atomic<bool> ota_busy, core_ready;
extern std::atomic<int64_t> ui_heartbeat, service_heartbeat;
extern Status state;
extern TaskHandle_t main_task_handle;
ESP_EVENT_DECLARE_BASE(EVENTS);
enum Event { SETTINGS = 1, BATTERY, WIFI, BLE, STORAGE, OTA, GATT };
struct GattEvent {
  int kind;
  uint16_t connection, handle, start, end;
  int status;
  uint16_t length;
  uint8_t data[512];
  char uuid[40];
};
class Guard {
  SemaphoreHandle_t m;

public:
  explicit Guard(SemaphoreHandle_t v) : m(v) { xSemaphoreTake(m, portMAX_DELAY); }
  ~Guard() { xSemaphoreGive(m); }
};
int64_t now_ms();
void publish(Event);
void notice(const char *);
Status snapshot();
esp_err_t config_init();
Config config();
SystemPrefs system_prefs();
uint16_t effective_sleep(const Battery &battery);
uint32_t setting_async(const char *key, int value);
uint32_t update_url_async(const char *url);
bool setting_result(SettingResult &result);
esp_err_t save_config(const Config &);
esp_err_t copy_config_to(const char *partition);
esp_err_t app_config_get(const char *id, uint32_t schema, void *data, size_t *size);
esp_err_t app_config_set(const char *id, uint32_t schema, const void *data, size_t size);
esp_err_t setting(const char *key, int value);
esp_err_t set_update_url(const char *);
void diag_set(const char *, const char *);
void diag_get(const char *, char *, size_t);
enum class Op {
  WIFI_SCAN,
  WIFI_CONNECT,
  WIFI_FORGET,
  WIFI_APPLY,
  BLE_APPLY,
  BLE_SCAN,
  BLE_CONNECT,
  BLE_DISCONNECT,
  BLE_FORGET,
  BLE_PAIR,
  AUDIO_TEST,
  SD_MOUNT,
  SD_UNMOUNT,
  OTA_SESSION,
  OTA_CHECK,
  OTA_INSTALL,
  REBOOT,
  SETTING,
  UPDATE_URL,
  TIME_SYNC,
  POWER_OFF,
  WIFI_SAVED_CONNECT,
  WIFI_PROFILE_FORGET,
  WIFI_PROFILE_AUTO
};
struct Command {
  Op op;
  uint32_t request_id = 0;
  int arg = 0;
  char a[384]{}, b[65]{};
};
bool command(Op op, const char *a = "", const char *b = "", int arg = 0);
void services_init();
void diagnostics_init();
esp_err_t board_init();
Battery battery_read();
void board_audio_test();
void board_volume();
int board_audio_mute_state();
bool board_key();
// Consume events sampled independently of LVGL: back=1, home=2, toggle=4, shutdown=8.
uint32_t board_key_events();
void board_shutdown();
void storage_mount();
void storage_unmount();
// Storage clients hold the lease across every file operation; unmount waits for
// leases.
extern SemaphoreHandle_t storage_mutex;
esp_err_t storage_list(const char *relative, size_t offset, size_t limit,
                       void (*entry)(const char *, void *), void *ctx);
esp_err_t storage_read(const char *relative, size_t offset, void *data, size_t size, size_t *got);
void wifi_init();
void wifi_scan();
void wifi_connect(const char *, const char *);
void wifi_apply();
void wifi_forget();
void wifi_tick();
void ble_init();
const char *ble_name();
void ble_apply();
void ble_scan();
void ble_connect(int, const char *expected_address = "");
void ble_disconnect(int, const char *expected_address = "");
void ble_forget(int, const char *expected_address = "");
void ble_pair();
void ble_tick();
void ble_data_ready(int conn, bool ready);
esp_err_t ble_read(int, uint16_t);
esp_err_t ble_write(int, uint16_t, const void *, size_t);
esp_err_t ble_subscribe(int, uint16_t);
void ota_init();
void ota_session();
void ota_check();
void ota_install();
void ota_boot_check();
struct DisplayMetrics {
  uint32_t actual_fps = 0, refresh_count = 0, flush_chunks = 0;
  uint64_t flushed_pixels = 0, refresh_work_total_us = 0, flush_submit_total_us = 0;
  uint32_t long_gap_count = 0, max_gap_us = 0;
  uint32_t frame_interval_samples = 0, refresh_p50_us = 0, refresh_p95_us = 0;
  uint32_t refresh_work_p95_us = 0, flush_wait_p95_us = 0;
};
void display_metrics_init();
DisplayMetrics display_metrics_snapshot();
void display_metrics_reset();
extern std::atomic<int> requested_scroll;
void ui_start();
void ui_home();
void ui_settings(lv_obj_t *);
void ui_battery(lv_obj_t *);
void ui_notifications(lv_obj_t *);
void ui_logs(lv_obj_t *);
void ui_logs_destroy();
using Create = void (*)(lv_obj_t *);
using Hook = void (*)();
struct App {
  const char *id;
  const char *name;
  const char *icon;
  uint32_t capabilities;
  Create create;
  Hook enter, leave, destroy;
};
extern const App apps[];
extern const size_t app_count;
void app_open(size_t);
void app_close();
extern "C" const lv_font_t font_zh;
} // namespace ya
