#include "notifications.hpp"
#include "storage_policy.hpp"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_partition.h"
#include "esp_spiffs.h"
#include "esp_vfs_fat.h"
#include "ya.hpp"
#include "power.hpp"
#include "service_internal.hpp"
#include "service_policy.hpp"
#include "wake_handoff.hpp"
#include <cmath>
#include <dirent.h>
#include <string>
#define XPOWERS_CHIP_AXP2101
#include "XPowersLib.h"
namespace ya {
static XPowersPMU pmu;
static i2c_master_dev_handle_t pmu_dev;
static bool io_error = false, pmu_ok = false;
static esp_codec_dev_handle_t speaker;
static std::atomic<int> last_volume{-1}, last_mute{-1};
static service_policy::Key back_key, home_key, power_key;
static std::atomic<uint32_t> key_events{0};
static void key_task(void *) {
  for (;;) {
    const auto now = now_ms();
    const auto wake = screen_sleeping.load() ? service_policy::KEY_WAKE : 0;
    auto events = back_key.sample(gpio_get_level(GPIO_NUM_0) == 0, now, 1, 0, wake) |
                  home_key.sample(gpio_get_level(GPIO_NUM_18) == 0, now, 2, 0, wake) |
                  power_key.sample(gpio_get_level(GPIO_NUM_16) != 0, now, 4, 8, wake);
    events = power_wake::route_keys(events, screen_sleeping.load(), power_request_wake);
    if (events)
      key_events.fetch_or(events);
    vTaskDelay(pdMS_TO_TICKS(16));
  }
}
static int read_pmu(uint8_t, uint8_t r, uint8_t *b, uint8_t n) {
  auto e = i2c_master_transmit_receive(pmu_dev, &r, 1, b, n, 100);
  if (e != ESP_OK)
    io_error = true;
  return e == ESP_OK ? 0 : -1;
}
static int write_pmu(uint8_t, uint8_t r, uint8_t *b, uint8_t n) {
  uint8_t out[256];
  out[0] = r;
  memcpy(out + 1, b, n);
  auto e = i2c_master_transmit(pmu_dev, out, n + 1, 100);
  if (e != ESP_OK)
    io_error = true;
  return e == ESP_OK ? 0 : -1;
}
esp_err_t board_init() {
  ESP_RETURN_ON_ERROR(bsp_i2c_init(), "board", "I2C");
  i2c_device_config_t dc{};
  dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  dc.device_address = 0x34;
  dc.scl_speed_hz = 400000;
  ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &dc, &pmu_dev), "board",
                      "PMU device");
  pmu_ok = pmu.begin(0x34, read_pmu, write_pmu);
  if (!pmu_ok)
    return ESP_ERR_NOT_FOUND;
  // Keep charging current/voltage and power rails as configured by the board.
  pmu.enableVbusVoltageMeasure();
  pmu.enableBattVoltageMeasure();
  pmu.enableSystemVoltageMeasure();
  pmu.enableTemperatureMeasure();
  pmu.setPowerKeyPressOnTime(XPOWERS_POWERON_128MS);
  pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_6S);
  pmu.setLongPressPowerOFF();
  pmu.enableLongPressShutdown();
  if (io_error)
    return ESP_FAIL;
  gpio_config_t gc{};
  // Viewed from the display with USB at the bottom: BOOT is left, IO18 right.
  // GPIO0 remains an input; the ROM download strap behavior is preserved.
  gc.pin_bit_mask = (1ULL << 0) | (1ULL << 18);
  gc.mode = GPIO_MODE_INPUT;
  gc.pull_up_en = GPIO_PULLUP_ENABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&gc), "board", "key");
  gc.pin_bit_mask = 1ULL << 16;
  gc.pull_up_en = GPIO_PULLUP_DISABLE;
  ESP_RETURN_ON_ERROR(gpio_config(&gc), "board", "power key");
  // Keep sampling while LVGL is paused. No GUI lock, I2C or storage work here.
  static StackType_t key_stack[2048];
  static StaticTask_t key_control;
  if (!xTaskCreateStatic(key_task, "keys", sizeof key_stack, nullptr, 5, key_stack,
                         &key_control))
    return ESP_ERR_NO_MEM;
  bsp_display_cfg_t display{};
  display.lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG();
  display.lv_adapter_cfg.task_stack_size = 12288;
  display.rotation = ESP_LV_ADAPTER_ROTATE_0;
  display.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE;
  display.touch_flags.swap_xy = 1;
  display.touch_flags.mirror_y = 1;
  if (!bsp_display_start_with_config(&display))
    return ESP_FAIL;
  ESP_RETURN_ON_ERROR(ui_boot_start(), "board", "startup display");
  speaker = bsp_audio_codec_speaker_init();
  if (speaker)
    board_volume();
  {
    Guard g(state_mutex);
    state.battery = battery_read();
  }
  // BSP/SPI contexts, internal draw buffers and the first DMA flush are ready.
  // Move ordinary startup/service objects to PSRAM before Wi-Fi/HTTP/data init;
  // explicit INTERNAL/DMA allocations and all task stacks keep their caps.
  heap_caps_malloc_extmem_enable(0);
  return ESP_OK;
}
Battery battery_read() {
  Battery b;
  b.sampled = now_ms();
  if (!pmu_ok)
    return b;
  io_error = false;
  b.present = pmu.isBatteryConnect();
  b.usb = pmu.isVbusIn();
  b.charging = pmu.isCharging();
  b.full = b.present && pmu.getChargerStatus() == XPOWERS_AXP2101_CHG_DONE_STATE;
  b.discharging = pmu.isDischarge();
  b.percent = b.present ? pmu.getBatteryPercent() : -1;
  b.millivolts = b.present ? pmu.getBattVoltage() : 0;
  b.temperature = pmu.getTemperature();
  b.valid = !io_error;
  if (b.percent < 0 || b.percent > 100)
    b.percent = -1;
  if (!b.present)
    b.charging = b.discharging = false;
  return b;
}
void board_volume() {
  auto c = config();
  int v = c.volume;
  if (speaker && last_volume != v) {
    if (esp_codec_dev_set_out_vol(speaker, v) == ESP_OK)
      last_volume = v;
  }
  int mute = c.mute || c.volume == 0;
  if (speaker && last_mute != mute && esp_codec_dev_set_out_mute(speaker, mute) == ESP_OK)
    last_mute = mute;
}
int board_audio_mute_state() { return last_mute.load(); }
void board_audio_test() {
  if (!speaker) {
    notice("音频设备不可用");
    return;
  }
  esp_codec_dev_sample_info_t fmt{};
  fmt.sample_rate = 22050;
  fmt.channel = 1;
  fmt.bits_per_sample = 16;
  if (esp_codec_dev_open(speaker, &fmt) != ESP_OK) {
    notice("音频初始化失败");
    return;
  }
  int16_t samples[256];
  for (int block = 0; block < 18; block++) {
    for (int i = 0; i < 256; i++)
      samples[i] = int16_t(5000.0 * sin(2 * 3.14159265 * 660 * (block * 256 + i) / 22050.0) *
                           sin(3.14159265 * (block * 256 + i) / (18 * 256)));
    esp_codec_dev_write(speaker, samples, sizeof(samples));
  }
  esp_codec_dev_close(speaker);
}
bool board_key() { return gpio_get_level(GPIO_NUM_18) == 0; }
uint32_t board_key_events() {
  return key_events.exchange(0);
}
static bool card_mounted() {
  Guard g(state_mutex);
  return state.sd_mounted;
}
void board_shutdown() {
  if (!notifications_checkpoint()) return;
  Guard mutation(mutation_mutex);
  bool pending;
  {
    Guard g(state_mutex);
    pending = state.pending;
  }
  if (ota_busy || pending) {
    notice("升级或启动验证中，暂不能关机");
    return;
  }
  if (!pmu_ok) {
    notice("电源管理不可用，无法关机");
    return;
  }
  // Finish existing storage leases before removing its power. The PMU's
  // independent six-second hold remains available as an emergency power cut.
  {
    Guard storage(storage_mutex);
    bool mounted;
    {
      Guard g(state_mutex);
      mounted = state.sd_mounted;
    }
    if (mounted) {
      auto e = esp_vfs_fat_sdcard_unmount("/sdcard", bsp_sdcard);
      if (e != ESP_OK) {
        notice("存储卸载失败，已取消关机");
        return;
      }
      Guard g(state_mutex);
      state.sd_mounted = false;
      state.sd_capacity_valid = false;
      state.sd_total = state.sd_free = 0;
    }
  }
  notice("正在关机");
  if (!notifications_checkpoint()) return;
  if (speaker)
    esp_codec_dev_set_out_mute(speaker, true);
  vTaskDelay(pdMS_TO_TICKS(150));
  io_error = false;
  pmu.shutdown();
  vTaskDelay(pdMS_TO_TICKS(150));
  // If execution continues, the rail did not turn off; do not pretend success.
  last_mute = -1;
  board_volume();
  notice(io_error ? "关机失败，电源管理通信错误" : "关机未完成，请检查电源管理");
}
static std::atomic<bool> flash_checked{false};
void storage_retry() { flash_checked = false; }
void board_service_tick() {
  static int64_t next_sample = 0;
  if (now_ms() < next_sample)
    return;
  next_sample = now_ms() + 5000;
  // Match shutdown: mutation must precede storage whenever both are needed.
  Guard mutation(mutation_mutex);
  Guard storage(storage_mutex);
  if (!flash_checked) {
    flash_checked = true;
    esp_vfs_spiffs_conf_t conf{};
    conf.base_path = "/spiffs";
    conf.partition_label = "storage";
    conf.max_files = 2;
    // Displaying free space never authorizes formatting an existing partition.
    conf.format_if_mount_failed = false;
    bool already_mounted;
    { Guard g(state_mutex); already_mounted = state.flash_mounted; }
    auto e = already_mounted ? ESP_OK : esp_vfs_spiffs_register(&conf);
    auto *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "storage");
    // Only a completely erased partition can be initialized automatically.
    // A failed read or any existing byte preserves the partition unchanged.
    if (e != ESP_OK && partition && !ota_busy) {
      auto contents = storage_policy::inspect(partition->size, [&](size_t offset, uint8_t *block, size_t n) {
        if (ota_busy) return false;
        auto result = esp_partition_read(partition, offset, block, n);
        taskYIELD(); return result == ESP_OK;
      });
      if (contents == storage_policy::Contents::ERASED && !ota_busy) {
        { Guard g(state_mutex); strlcpy(state.flash_note, "空白分区，正在初始化", sizeof state.flash_note); }
        conf.format_if_mount_failed = true; e = esp_vfs_spiffs_register(&conf);
      }
    }
    Guard g(state_mutex);
    state.flash_error = e;
    strlcpy(state.flash_note, e == ESP_OK ? "已挂载" : "挂载失败，已保留原数据", sizeof state.flash_note);
    state.flash_mounted = e == ESP_OK;
    state.flash_total = partition ? partition->size : 0;
  }
  bool sd, flash;
  {
    Guard g(state_mutex);
    sd = state.sd_mounted;
    flash = state.flash_mounted;
  }
  uint64_t sd_total = 0, sd_free = 0;
  size_t flash_total = 0, flash_used = 0;
  auto se = sd ? esp_vfs_fat_info("/sdcard", &sd_total, &sd_free) : ESP_ERR_INVALID_STATE;
  auto fe = flash ? esp_spiffs_info("storage", &flash_total, &flash_used) : ESP_ERR_INVALID_STATE;
  {
    Guard g(state_mutex);
    if (se == ESP_OK) {
      state.sd_capacity_valid = true;
      state.sd_total = sd_total;
      state.sd_free = sd_free;
    } else
      state.sd_capacity_valid = false;
    if (fe == ESP_OK) {
      state.flash_capacity_valid = true;
      state.flash_total = flash_total;
      state.flash_free = flash_used <= flash_total ? flash_total - flash_used : 0;
    } else {
      state.flash_capacity_valid = false;
      if (flash) strlcpy(state.flash_note, "已挂载，容量读取失败", sizeof state.flash_note);
    }
  }
}
void storage_mount() {
  Guard lease(storage_mutex);
  if (card_mounted())
    return;
  auto e = bsp_sdcard_mount();
  uint64_t total = 0, free = 0;
  auto capacity = e == ESP_OK ? esp_vfs_fat_info("/sdcard", &total, &free) : e;
  {
    Guard g(state_mutex);
    state.sd_mounted = e == ESP_OK;
    state.sd_capacity_valid = capacity == ESP_OK;
    state.sd_total = total;
    state.sd_free = free;
  }
  notice(e == ESP_OK ? "TF 卡已挂载" : "未发现 TF 卡或文件系统不可读");
  publish(STORAGE);
}
void storage_unmount() {
  Guard lease(storage_mutex);
  if (!card_mounted())
    return;
  auto e = esp_vfs_fat_sdcard_unmount("/sdcard", bsp_sdcard);
  if (e == ESP_OK) {
    Guard g(state_mutex);
    state.sd_mounted = false;
    state.sd_capacity_valid = false;
    state.sd_total = state.sd_free = 0;
  }
  notice(e == ESP_OK ? "TF 卡已安全卸载" : "卸载失败");
  publish(STORAGE);
}
static bool valid_path(const char *p) {
  return p && strlen(p) < 240 && *p != '/' && !strstr(p, "..") && !strchr(p, '\\');
}
esp_err_t storage_list(const char *rel, size_t offset, size_t limit,
                       void (*entry)(const char *, void *), void *ctx) {
  if (!valid_path(rel) || !entry || limit > 100)
    return ESP_ERR_INVALID_ARG;
  Guard lease(storage_mutex);
  if (!card_mounted())
    return ESP_ERR_INVALID_STATE;
  auto path = std::string("/sdcard/") + rel;
  DIR *d = opendir(path.c_str());
  if (!d)
    return ESP_FAIL;
  size_t i = 0, n = 0;
  dirent *e;
  while ((e = readdir(d)) && n < limit) {
    if (e->d_name[0] == '.')
      continue;
    if (i++ < offset)
      continue;
    entry(e->d_name, ctx);
    n++;
  }
  closedir(d);
  return ESP_OK;
}
esp_err_t storage_read(const char *rel, size_t offset, void *data, size_t size, size_t *got) {
  if (!valid_path(rel) || !data || !got || size > 65536)
    return ESP_ERR_INVALID_ARG;
  *got = 0;
  Guard lease(storage_mutex);
  if (!card_mounted())
    return ESP_ERR_INVALID_STATE;
  auto path = std::string("/sdcard/") + rel;
  FILE *f = fopen(path.c_str(), "rb");
  if (!f)
    return ESP_FAIL;
  if (fseek(f, offset, SEEK_SET)) {
    fclose(f);
    return ESP_FAIL;
  }
  *got = fread(data, 1, size, f);
  bool bad = ferror(f);
  fclose(f);
  return bad ? ESP_FAIL : ESP_OK;
}
} // namespace ya
