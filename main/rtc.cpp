#include "ya.hpp"
#include "service_internal.hpp"
#include "service_policy.hpp"
#include "bsp/esp-bsp.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include <cstdlib>
#include <ctime>
#include <sys/time.h>

namespace ya {
static i2c_master_dev_handle_t rtc;
static constexpr uint8_t UTC_MARKER = 0xa7;
static constexpr uint8_t STOP = 0x20, HOUR_12 = 0x02, EXT_TEST = 0x80;
static std::atomic<int64_t> synced_epoch{0};
static bool was_connected = false, manual_sync = false;
static int64_t sync_deadline = 0, retry_at = 0;
static unsigned server_index = 0;
static const char *const servers[] = {"ntp.aliyun.com", "pool.ntp.org", "time.cloudflare.com"};

static esp_err_t rtc_read(uint8_t reg, void *data, size_t size) {
  return rtc ? i2c_master_transmit_receive(rtc, &reg, 1, static_cast<uint8_t *>(data), size, 100)
             : ESP_ERR_NOT_FOUND;
}
static esp_err_t rtc_byte(uint8_t reg, uint8_t value) {
  uint8_t bytes[] = {reg, value};
  return rtc ? i2c_master_transmit(rtc, bytes, sizeof(bytes), 100) : ESP_ERR_NOT_FOUND;
}
static bool clock_range(int64_t epoch) { return epoch >= 1704067200 && epoch < 4102444800LL; }
static esp_err_t rtc_time(int64_t &epoch, bool require_marker = true) {
  uint8_t control = 0, marker = 0, calendar[7];
  auto e = rtc_read(0, &control, 1);
  if (e == ESP_OK)
    e = rtc_read(3, &marker, 1);
  if (e == ESP_OK)
    // NXP requires seconds through years in one access (maximum one second).
    e = rtc_read(4, calendar, sizeof(calendar));
  if (e != ESP_OK)
    return e;
  if ((control & (STOP | HOUR_12 | EXT_TEST)) || (require_marker && marker != UTC_MARKER) ||
      !service_policy::rtc_epoch(calendar, epoch) || !clock_range(epoch))
    return ESP_ERR_INVALID_STATE;
  return ESP_OK;
}
static esp_err_t rtc_store_now() {
  uint8_t control = 0;
  auto e = rtc_read(0, &control, 1);
  if (e != ESP_OK)
    return e;
  // Keep load-capacitance/correction configuration. Never reset a running RTC.
  uint8_t running = control & 0x05;
  e = rtc_byte(3, 0); // A partial write must never appear to be a valid UTC clock.
  if (e != ESP_OK)
    return e;
  e = rtc_byte(0, running | STOP);
  time_t now = time(nullptr);
  struct tm utc{};
  if (e == ESP_OK && (!clock_range(now) || !gmtime_r(&now, &utc)))
    e = ESP_ERR_INVALID_ARG;
  if (e == ESP_OK) {
    uint8_t bytes[] = {4,
                       service_policy::to_bcd(utc.tm_sec), // Writing clears OS.
                       service_policy::to_bcd(utc.tm_min),
                       service_policy::to_bcd(utc.tm_hour),
                       service_policy::to_bcd(utc.tm_mday),
                       uint8_t(utc.tm_wday),
                       service_policy::to_bcd(utc.tm_mon + 1),
                       service_policy::to_bcd(utc.tm_year - 100)};
    e = i2c_master_transmit(rtc, bytes, sizeof(bytes), 100);
  }
  // Always try to release STOP, including a failed calendar transfer.
  auto resume = rtc_byte(0, running);
  if (e == ESP_OK)
    e = resume;
  int64_t readback = 0;
  if (e == ESP_OK)
    e = rtc_time(readback, false);
  if (e == ESP_OK && (readback < now || readback > now + 1))
    e = ESP_ERR_INVALID_RESPONSE;
  if (e == ESP_OK)
    e = rtc_byte(3, UTC_MARKER);
  return e;
}
static void sync_callback(struct timeval *tv) {
  // The lwIP callback must not do I2C, flash I/O, or take the GUI lock.
  synced_epoch.store(tv->tv_sec);
}
static bool network_connected() {
  Guard g(state_mutex);
  return state.wifi_connected;
}
static void begin_sync() {
  if (esp_sntp_enabled())
    esp_sntp_stop();
  esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, servers[server_index]);
  esp_sntp_init();
  sync_deadline = now_ms() + 45000;
  retry_at = 0;
  Guard g(state_mutex);
  state.time_syncing = true;
  strlcpy(state.time_note, "正在同步时间", sizeof(state.time_note));
}

void time_service_init() {
  setenv("TZ", "CST-8", 1);
  tzset();
  i2c_device_config_t dc{};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = 0x51;
  dc.scl_speed_hz = 400000;
  auto e = i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &rtc);
  int64_t epoch = 0;
  if (e == ESP_OK)
    e = rtc_time(epoch);
  if (e == ESP_OK) {
    timeval tv{};
    tv.tv_sec = epoch;
    if (settimeofday(&tv, nullptr) != 0)
      e = ESP_FAIL;
  }
  {
    Guard g(state_mutex);
    state.time_valid = state.rtc_valid = e == ESP_OK;
    strlcpy(state.time_note, e == ESP_OK ? "已从 RTC 恢复时间" : "时间未同步，请连接网络",
            sizeof(state.time_note));
  }
  esp_sntp_set_time_sync_notification_cb(sync_callback);
  esp_sntp_set_sync_interval(60 * 60 * 1000);
}
void time_service_request_sync() {
  if (!network_connected()) {
    notice("请先连接网络再同步时间");
    return;
  }
  manual_sync = true;
  was_connected = true;
  server_index = 0;
  begin_sync();
  notice("正在同步时间");
}
void time_service_tick() {
  int64_t synchronized = synced_epoch.exchange(0);
  if (synchronized) {
    if (clock_range(synchronized)) {
      auto e = rtc_store_now();
      sync_deadline = retry_at = 0;
      {
        Guard g(state_mutex);
        state.time_valid = true;
        state.time_syncing = false;
        state.rtc_valid = e == ESP_OK;
        state.last_time_sync = synchronized;
        strlcpy(state.time_note,
                e == ESP_OK ? "时间已同步，RTC 已保存" : "时间已同步，RTC 保存失败",
                sizeof(state.time_note));
      }
      if (manual_sync)
        notice(e == ESP_OK ? "时间同步成功" : "时间同步成功，RTC 保存失败");
      manual_sync = false;
      if (e != ESP_OK)
        ESP_LOGW("clock", "RTC write failed: %s", esp_err_to_name(e));
      publish(SETTINGS);
    } else {
      if (esp_sntp_enabled())
        esp_sntp_stop();
      sync_deadline = 0;
      retry_at = now_ms() + 60000;
      {
        Guard g(state_mutex);
        state.time_valid = false;
        state.time_syncing = false;
        strlcpy(state.time_note, "时间服务器返回无效日期", sizeof(state.time_note));
      }
      notice("时间服务器返回无效日期");
    }
  }
  bool online = network_connected();
  if (!online) {
    if (was_connected) {
      if (esp_sntp_enabled())
        esp_sntp_stop();
      sync_deadline = retry_at = 0;
      Guard g(state_mutex);
      state.time_syncing = false;
      strlcpy(state.time_note, state.time_valid ? "离线，使用本地时间" : "时间未同步，请连接网络",
              sizeof(state.time_note));
    }
    was_connected = false;
    return;
  }
  if (!was_connected || (retry_at && now_ms() >= retry_at)) {
    was_connected = true;
    begin_sync();
  } else if (sync_deadline && now_ms() >= sync_deadline) {
    esp_sntp_stop();
    sync_deadline = 0;
    retry_at = now_ms() + 60000;
    server_index = (server_index + 1) % (sizeof(servers) / sizeof(*servers));
    {
      Guard g(state_mutex);
      state.time_syncing = false;
      strlcpy(state.time_note, "时间同步超时，将自动重试", sizeof(state.time_note));
    }
    if (manual_sync)
      notice("时间同步超时，将自动重试");
    manual_sync = false;
  }
}
} // namespace ya
