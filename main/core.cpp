#include "notifications.hpp"
#include "logs.hpp"
#include <cctype>
#include "esp_check.h"
#include "ya.hpp"
#include "service_internal.hpp"
#include "data.hpp"
#include "power.hpp"
#include "service_policy.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
namespace ya {
ESP_EVENT_DEFINE_BASE(EVENTS);
SemaphoreHandle_t state_mutex, config_mutex, storage_mutex, mutation_mutex;
Status state;
std::atomic<bool> ota_busy{false}, core_ready{false};
std::atomic<int64_t> ui_heartbeat{0}, service_heartbeat{0};
static Config current;
static SystemPrefs preferences;
static QueueHandle_t commands, setting_results;
static SemaphoreHandle_t result_slots, app_store_mutex;
static std::atomic<uint32_t> next_request{0};
static std::atomic<bool> wifi_apply_pending{false}, ble_apply_pending{false};
static constexpr unsigned SETTING_CAPACITY = 12;
static constexpr const char *PREFS_KEY = "sys_prefs";
struct PrefsRecord {
  uint32_t schema = 2;
  uint32_t migrated = 0x59415032;
  SystemPrefs values;
};
static_assert(sizeof(Config) == 496, "The 0.1.1 v1 NVS ABI must stay unchanged");
static_assert(sizeof(PrefsRecord) == 12, "Persistent preferences layout changed");
static const char *ns(const char *p) { return strcmp(p, "ota_1") == 0 ? "config_b" : "config_a"; }
static const char *app_ns(const char *slot) { return !strcmp(slot, "ota_1") ? "apps_b" : "apps_a"; }
int64_t now_ms() { return esp_timer_get_time() / 1000; }
void publish(Event e) { esp_event_post(EVENTS, e, nullptr, 0, 0); }
void notice(const char *s) {
  notifications_add(s);
  publish(SETTINGS);
}
Status snapshot() {
  Guard g(state_mutex);
  return state;
}
Config config() {
  Guard g(config_mutex);
  return current;
}
SystemPrefs system_prefs() {
  Guard g(config_mutex);
  return preferences;
}
uint16_t effective_sleep(const Battery &battery) {
  auto p = system_prefs();
  // A stale or failed PMU reading must not accidentally select USB always-on.
  return battery.valid && now_ms() - battery.sampled < 5000 && battery.usb ? p.sleep_usb
                                                                           : p.sleep_battery;
}
static bool valid(const Config &c) {
  return c.schema == 1 && c.brightness >= 5 && c.brightness <= 100 && c.volume <= 100 &&
         (c.fps == 15 || c.fps == 30 || c.fps == 60) &&
         (c.sleep_seconds == 0 || c.sleep_seconds == 30 || c.sleep_seconds == 60 ||
          c.sleep_seconds == 300) &&
         memchr(c.ssid, 0, sizeof(c.ssid)) && memchr(c.password, 0, sizeof(c.password)) &&
         memchr(c.update_url, 0, sizeof(c.update_url));
}
static esp_err_t store(const Config &c, const char *name) {
  nvs_handle_t h;
  esp_err_t e = nvs_open(name, NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  e = nvs_set_blob(h, "v1", &c, sizeof(c));
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
static esp_err_t store_prefs(const SystemPrefs &p, const char *slot) {
  Guard storage(app_store_mutex);
  nvs_handle_t h;
  auto e = nvs_open(app_ns(slot), NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  PrefsRecord record;
  record.values = p;
  e = nvs_set_blob(h, PREFS_KEY, &record, sizeof(record));
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t config_init() {
  state_mutex = xSemaphoreCreateMutex();
  config_mutex = xSemaphoreCreateMutex();
  storage_mutex = xSemaphoreCreateMutex();
  mutation_mutex = xSemaphoreCreateMutex();
  app_store_mutex = xSemaphoreCreateMutex();
  ESP_RETURN_ON_FALSE(state_mutex && config_mutex && storage_mutex && mutation_mutex &&
                          app_store_mutex,
                      ESP_ERR_NO_MEM, "core", "mutex");
  esp_err_t e = nvs_flash_init();
  if (e != ESP_OK)
    return e;
  nvs_handle_t h;
  const char *slot = esp_ota_get_running_partition()->label;
  e = nvs_open(ns(slot), NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  Config c;
  size_t len = sizeof(c);
  e = nvs_get_blob(h, "v1", &c, &len);
  nvs_close(h);
  bool new_config = e == ESP_ERR_NVS_NOT_FOUND;
  if (!new_config && (e != ESP_OK || len != sizeof(c) || !valid(c)))
    return ESP_ERR_INVALID_STATE;
  current = new_config ? Config{} : c;
  // Keep new data in the namespace which even 0.1.1 clears and copies on OTA.
  // An old installer only overwrites config_*/v1; a separate v2 key there could
  // resurrect preferences from a previously failed installation.
  PrefsRecord record;
  e = nvs_open(app_ns(slot), NVS_READONLY, &h);
  if (e == ESP_OK) {
    len = sizeof(record);
    e = nvs_get_blob(h, PREFS_KEY, &record, &len);
    nvs_close(h);
  }
  if (e == ESP_ERR_NVS_NOT_FOUND) {
    preferences = SystemPrefs{};
    strlcpy(current.update_url, UPDATE_MANIFEST_URL, sizeof(current.update_url));
    ESP_RETURN_ON_ERROR(store(current, ns(slot)), "core", "v1 migration");
    return store_prefs(preferences, slot);
  }
  if (e != ESP_OK || len != sizeof(record) || record.schema != 2 || record.migrated != 0x59415032 ||
      !service_policy::battery_timeout(record.values.sleep_battery) ||
      !service_policy::usb_timeout(record.values.sleep_usb))
    return ESP_ERR_INVALID_STATE;
  preferences = record.values;
  if (new_config) {
    strlcpy(current.update_url, UPDATE_MANIFEST_URL, sizeof(current.update_url));
    return store(current, ns(slot));
  }
  return ESP_OK;
}
esp_err_t save_config(const Config &c) {
  if (!valid(c))
    return ESP_ERR_INVALID_ARG;
  // Callers serialize mutations. Do not keep the GUI's snapshot lock across
  // flash erase/commit, which can take hundreds of milliseconds.
  auto e = store(c, ns(esp_ota_get_running_partition()->label));
  if (e == ESP_OK) {
    Guard g(config_mutex);
    current = c;
  }
  publish(SETTINGS);
  return e;
}
static bool app_key(const char *id) {
  if (!id || !*id || strlen(id) > 15 || (!strcmp(id, PREFS_KEY) || !strcmp(id, "sys_wifi")))
    return false;
  for (const char *p = id; *p; p++)
    if (!isalnum((unsigned char)*p) && *p != '_' && *p != '-')
      return false;
  return true;
}
esp_err_t app_config_get(const char *id, uint32_t schema, void *data, size_t *size) {
  if (!app_key(id) || !size || !data || *size > 4096)
    return ESP_ERR_INVALID_ARG;
  Guard g(app_store_mutex);
  nvs_handle_t h;
  auto e = nvs_open(app_ns(esp_ota_get_running_partition()->label), NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;
  uint8_t *blob = (uint8_t *)malloc(4100);
  if (!blob) {
    nvs_close(h);
    return ESP_ERR_NO_MEM;
  }
  size_t len = 4100;
  e = nvs_get_blob(h, id, blob, &len);
  if (e == ESP_OK) {
    uint32_t stored = 0;
    if (len >= 4)
      memcpy(&stored, blob, 4);
    if (len < 4 || stored != schema)
      e = ESP_ERR_INVALID_VERSION;
    else if (*size < len - 4) {
      *size = len - 4;
      e = ESP_ERR_INVALID_SIZE;
    } else {
      *size = len - 4;
      memcpy(data, blob + 4, *size);
    }
  }
  free(blob);
  nvs_close(h);
  return e;
}
esp_err_t app_config_set(const char *id, uint32_t schema, const void *data, size_t size) {
  if (!app_key(id) || !data || size > 4096)
    return ESP_ERR_INVALID_ARG;
  Guard m(mutation_mutex);
  if (ota_busy)
    return ESP_ERR_INVALID_STATE;
  Guard g(app_store_mutex);
  nvs_handle_t h;
  auto e = nvs_open(app_ns(esp_ota_get_running_partition()->label), NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  auto *blob = (uint8_t *)malloc(size + 4);
  if (!blob) {
    nvs_close(h);
    return ESP_ERR_NO_MEM;
  }
  memcpy(blob, &schema, 4);
  memcpy(blob + 4, data, size);
  e = nvs_set_blob(h, id, blob, size + 4);
  if (e == ESP_OK)
    e = nvs_commit(h);
  free(blob);
  nvs_close(h);
  return e;
}
esp_err_t wifi_store_read(void *data, size_t *size) {
  Guard g(app_store_mutex);
  nvs_handle_t h;
  auto e = nvs_open(app_ns(esp_ota_get_running_partition()->label), NVS_READONLY, &h);
  if (e != ESP_OK)
    return e;
  e = nvs_get_blob(h, "sys_wifi", data, size);
  nvs_close(h);
  return e;
}
esp_err_t wifi_store_write(const void *data, size_t size) {
  if (size > 4096)
    return ESP_ERR_INVALID_SIZE;
  Guard g(app_store_mutex);
  nvs_handle_t h;
  auto e = nvs_open(app_ns(esp_ota_get_running_partition()->label), NVS_READWRITE, &h);
  if (e != ESP_OK)
    return e;
  e = nvs_set_blob(h, "sys_wifi", data, size);
  if (e == ESP_OK)
    e = nvs_commit(h);
  nvs_close(h);
  return e;
}
esp_err_t copy_config_to(const char *p) {
  if (!p || (strcmp(p, "ota_0") && strcmp(p, "ota_1")) ||
      !strcmp(p, esp_ota_get_running_partition()->label))
    return ESP_ERR_INVALID_ARG;
  auto c = config();
  auto e = store(c, ns(p));
  if (e != ESP_OK)
    return e;
  Guard g(app_store_mutex);
  nvs_handle_t src, dst;
  e = nvs_open(app_ns(p), NVS_READWRITE, &dst);
  if (e != ESP_OK)
    return e;
  e = nvs_erase_all(dst);
  auto se = nvs_open(app_ns(esp_ota_get_running_partition()->label), NVS_READONLY, &src);
  if (se == ESP_OK && e == ESP_OK) {
    nvs_iterator_t it = nullptr;
    auto ie =
        nvs_entry_find("nvs", app_ns(esp_ota_get_running_partition()->label), NVS_TYPE_BLOB, &it);
    while (ie == ESP_OK && e == ESP_OK) {
      nvs_entry_info_t info;
      nvs_entry_info(it, &info);
      size_t len = 0;
      e = nvs_get_blob(src, info.key, nullptr, &len);
      if (e != ESP_OK || len > 4100) {
        e = ESP_ERR_INVALID_SIZE;
        break;
      }
      auto *buf = malloc(len);
      if (!buf) {
        e = ESP_ERR_NO_MEM;
        break;
      }
      e = nvs_get_blob(src, info.key, buf, &len);
      if (e == ESP_OK)
        e = nvs_set_blob(dst, info.key, buf, len);
      free(buf);
      ie = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    nvs_close(src);
    if (ie != ESP_OK && ie != ESP_ERR_NVS_NOT_FOUND)
      e = ie;
  } else if (se != ESP_OK && se != ESP_ERR_NVS_NOT_FOUND)
    e = se;
  else if (se == ESP_OK)
    nvs_close(src);
  if (e == ESP_OK)
    e = nvs_commit(dst);
  nvs_close(dst);
  return e;
}
esp_err_t setting(const char *k, int v) {
  if (!k)
    return ESP_ERR_INVALID_ARG;
  if (!strcmp(k, "log_level"))
    return logs_setting(v);
  if (!strncmp(k, "power_", 6))
    return power_setting(k, v);
  Guard mutation(mutation_mutex);
  if (ota_busy)
    return ESP_ERR_INVALID_STATE;
  if (!strcmp(k, "sleep"))
    return ESP_ERR_NOT_SUPPORTED;
  if (!strcmp(k, "sleep_battery") || !strcmp(k, "sleep_usb")) {
    bool battery = !strcmp(k, "sleep_battery");
    if (!(battery ? service_policy::battery_timeout(v) : service_policy::usb_timeout(v)))
      return ESP_ERR_INVALID_ARG;
    auto p = system_prefs();
    (battery ? p.sleep_battery : p.sleep_usb) = v;
    auto e = store_prefs(p, esp_ota_get_running_partition()->label);
    if (e == ESP_OK) {
      Guard g(config_mutex);
      preferences = p;
    }
    publish(SETTINGS);
    return e;
  }
  auto c = config();
  if (!strcmp(k, "brightness") && v >= 5 && v <= 100)
    c.brightness = v;
  else if (!strcmp(k, "volume") && v >= 0 && v <= 100)
    c.volume = v;
  else if (!strcmp(k, "mute") && (v == 0 || v == 1))
    c.mute = v;
  else if (!strcmp(k, "fps") && (v == 15 || v == 30 || v == 60))
    c.fps = v;
  else if (!strcmp(k, "wifi") && (v == 0 || v == 1))
    c.wifi = v;
  else if (!strcmp(k, "ble") && (v == 0 || v == 1))
    c.ble = v;
  else
    return ESP_ERR_INVALID_ARG;
  auto e = save_config(c);
  if (e == ESP_OK) {
    if (!strcmp(k, "wifi"))
      wifi_apply_pending = true;
    if (!strcmp(k, "ble"))
      ble_apply_pending = true;
  }
  return e;
}
esp_err_t set_update_url(const char *s) {
  Guard mutation(mutation_mutex);
  if (ota_busy)
    return ESP_ERR_INVALID_STATE;
  if (!s || strlen(s) >= sizeof(current.update_url) || strncmp(s, "https://", 8) || !s[8])
    return ESP_ERR_INVALID_ARG;
  auto c = config();
  strlcpy(c.update_url, s, sizeof(c.update_url));
  return save_config(c);
}
void diag_set(const char *k, const char *v) {
  nvs_handle_t h;
  if (nvs_open("diagnostic", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_str(h, k, v);
    nvs_commit(h);
    nvs_close(h);
  }
}
void diag_get(const char *k, char *v, size_t n) {
  *v = 0;
  nvs_handle_t h;
  if (nvs_open("diagnostic", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_str(h, k, v, &n);
    nvs_close(h);
  }
}
bool command(Op op, const char *a, const char *b, int arg) {
  if (!commands || !a || !b || strlen(a) >= 384 || strlen(b) >= 65)
    return false;
  Command c{};
  c.op = op;
  c.arg = arg;
  strlcpy(c.a, a, sizeof(c.a));
  strlcpy(c.b, b, sizeof(c.b));
  return xQueueSend(commands, &c, 0) == pdTRUE;
}
static uint32_t async_request(Op op, const char *value, int arg) {
  if (!commands || !value || strlen(value) >= sizeof(Command::a) ||
      xSemaphoreTake(result_slots, 0) != pdTRUE)
    return 0;
  Command c{};
  c.op = op;
  do {
    c.request_id = ++next_request;
  } while (!c.request_id);
  c.arg = arg;
  strlcpy(c.a, value, sizeof(c.a));
  if (xQueueSend(commands, &c, 0) != pdTRUE) {
    xSemaphoreGive(result_slots);
    return 0;
  }
  return c.request_id;
}
uint32_t setting_async(const char *key, int value) {
  return async_request(Op::SETTING, key, value);
}
uint32_t update_url_async(const char *url) { return async_request(Op::UPDATE_URL, url, 0); }
bool setting_result(SettingResult &result) {
  if (!setting_results || xQueueReceive(setting_results, &result, 0) != pdTRUE)
    return false;
  xSemaphoreGive(result_slots);
  return true;
}
static void complete(const Command &c, esp_err_t error) {
  if (!c.request_id)
    return;
  SettingResult result{c.request_id, error};
  // Every accepted request reserved one queue slot until the GUI consumes it.
  // Never block the service task or silently discard a completion on overload.
  BaseType_t sent = xQueueSend(setting_results, &result, 0);
  configASSERT(sent == pdTRUE);
}
static void sample_memory() {
  constexpr unsigned internal = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  constexpr unsigned external = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  const uint32_t ram_free = heap_caps_get_free_size(internal);
  const uint32_t ram_total = heap_caps_get_total_size(internal);
  const uint32_t psram_free = heap_caps_get_free_size(external);
  const uint32_t psram_total = heap_caps_get_total_size(external);
  const uint32_t minimum = heap_caps_get_minimum_free_size(internal);
  const uint32_t largest = heap_caps_get_largest_free_block(internal);
  Guard g(state_mutex);
  state.ram_free = ram_free;
  state.ram_total = ram_total;
  state.psram_free = psram_free;
  state.psram_total = psram_total;
  state.internal_min = minimum;
  state.internal_largest = largest;
}
static void worker(void *) {
  int64_t sampled = 0;
  int64_t battery_sampled = -60000;
  static battery_stats::Tracker battery_tracker;
  Command c;
  for (;;) {
    service_heartbeat = now_ms();
    // Keep light-sleep monitoring on the existing worker with sparse PMU reads.
    // Deep sleep reboots the tracker; never bridge an unobserved power-source change.
    if (now_ms() - battery_sampled >= (power_paused ? 60000 : 1000)) {
      auto b = battery_read();
      battery_sampled = b.sampled;
      b.stats = battery_tracker.update({b.sampled, b.percent, b.valid, b.present, b.usb,
                                        b.charging, b.discharging, b.full});
      {
        Guard g(state_mutex);
        state.battery = b;
      }
      publish(BATTERY);
    }
    if (power_paused) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    if (xQueueReceive(commands, &c, pdMS_TO_TICKS(50))) {
      if (ota_busy && c.op != Op::OTA_CHECK && c.op != Op::TIME_SYNC) {
        complete(c, ESP_ERR_INVALID_STATE);
        notice("升级中，请稍候");
      } else
        switch (c.op) {
        case Op::WIFI_SCAN:
          wifi_scan();
          break;
        case Op::WIFI_CONNECT:
          wifi_connect_request(c.a, c.b, c.arg);
          break;
        case Op::WIFI_FORGET:
          wifi_forget();
          break;
        case Op::WIFI_SAVED_CONNECT:
          wifi_connect_saved(c.arg);
          break;
        case Op::WIFI_PROFILE_FORGET:
          wifi_forget_id(c.arg);
          break;
        case Op::WIFI_PROFILE_AUTO:
          wifi_profile_auto(c.arg, c.a[0] == '1');
          break;
        case Op::WIFI_APPLY:
          wifi_apply();
          break;
        case Op::BLE_APPLY:
          ble_apply();
          break;
        case Op::BLE_SCAN:
          ble_scan();
          break;
        case Op::BLE_CONNECT:
          ble_connect(c.arg, c.a);
          break;
        case Op::BLE_DISCONNECT:
          ble_disconnect(c.arg, c.a);
          break;
        case Op::BLE_FORGET:
          ble_forget(c.arg, c.a);
          break;
        case Op::BLE_PAIR:
          ble_pair();
          break;
        case Op::AUDIO_TEST:
          board_audio_test();
          break;
        case Op::SD_MOUNT:
          storage_mount();
          break;
        case Op::SD_UNMOUNT:
          storage_unmount();
          break;
        case Op::OTA_SESSION:
          ota_session();
          break;
        case Op::OTA_CHECK:
          ota_check();
          break;
        case Op::OTA_INSTALL:
          ota_install();
          break;
        case Op::REBOOT: {
          Guard mutation(mutation_mutex);
          bool pending;
          {
            Guard g(state_mutex);
            pending = state.pending;
          }
          if (ota_busy || pending)
            notice("升级或启动验证中，暂不能重启");
          else if (notifications_checkpoint())
            esp_restart();
          break;
        }
        case Op::SETTING:
          complete(c, setting(c.a, c.arg));
          break;
        case Op::UPDATE_URL:
          complete(c, set_update_url(c.a));
          break;
        case Op::TIME_SYNC:
          time_service_request_sync();
          break;
        case Op::POWER_OFF:
          board_shutdown();
          break;
        }
    }
    if (!ota_busy) {
      if (wifi_apply_pending.exchange(false)) {
        wifi_apply();
        if (ota_busy)
          wifi_apply_pending = true;
      }
      if (ble_apply_pending.exchange(false)) {
        ble_apply();
        if (ota_busy)
          ble_apply_pending = true;
      }
    }
    notifications_tick();
    time_service_tick();
    if (now_ms() - sampled >= 1000) {
      sampled = now_ms();
      board_volume();
      wifi_tick();
      ble_tick();
      sample_memory();
      board_service_tick();
    }
  }
}
void services_init() {
  commands = xQueueCreate(12, sizeof(Command));
  setting_results = xQueueCreate(SETTING_CAPACITY, sizeof(SettingResult));
  result_slots = xSemaphoreCreateCounting(SETTING_CAPACITY, SETTING_CAPACITY);
  if (!commands || !setting_results || !result_slots)
    abort();
  notifications_init();
  time_service_init();
  wifi_init(); // Initializes lwIP before the OTA HTTP server opens sockets.
  // Reserve the OTA worker's contiguous internal stack before BLE/data tasks
  // fragment the internal heap. Flash work must never use a PSRAM stack.
  ota_init();
  data_init();
  ble_init();
  static StackType_t stack[8192];
  static StaticTask_t task;
  if (!xTaskCreateStatic(worker, "services", sizeof stack, nullptr, 5, stack, &task))
    abort();
}
} // namespace ya
