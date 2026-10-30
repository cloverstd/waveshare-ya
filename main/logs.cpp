#include "logs.hpp"
#include "log_assembler.hpp"
#include "esp_random.h"
#include "ya.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "nvs.h"
#include <cstdarg>
#include <new>

static_assert(CONFIG_LOG_VERSION == 1, "Log capture requires the pinned Log V1 format");
static_assert(CONFIG_LOG_MAXIMUM_LEVEL == 5, "Compile Debug/Verbose for runtime selection");
static_assert(CONFIG_LOG_DEFAULT_LEVEL == ya::LOG_DEFAULT_LEVEL, "Default must be Warning");
static_assert(CONFIG_LOG_DYNAMIC_LEVEL_CONTROL, "Log app requires runtime level control");

namespace ya {
namespace {
LogBuffer *buffer = nullptr;
LogAssembler assembler;
uint32_t boot_id = 0;
SemaphoreHandle_t buffer_mutex = nullptr, level_mutex = nullptr;
std::atomic<int> threshold{LOG_DEFAULT_LEVEL};
std::atomic<uint32_t> dropped{0};
vprintf_like_t console_output = vprintf;
int capture(const char *format, va_list args) {
  // No application allocations, GUI, storage, recursive logging or waits here.
  char text[LOG_TEXT_SIZE]{};
  va_list copy;
  va_copy(copy, args);
  int n = vsnprintf(text, sizeof(text), format, copy);
  va_end(copy);
  if (n > 0) {
    log_clean(text, sizeof(text), n);
    if (xSemaphoreTake(buffer_mutex, 0) == pdTRUE) {
      assembler.feed(reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle()), text,
                     now_ms(), threshold.load(), *buffer);
      xSemaphoreGive(buffer_mutex);
    } else ++dropped;
  }
  return console_output(format, args); // retain the existing serial destination
}
void apply(int level) {
  threshold = level;
  esp_log_level_set("*", static_cast<esp_log_level_t>(level));
}
} // namespace
uint32_t logs_boot_id() { return boot_id; }
void logs_init() {
  boot_id = esp_random() & 0x7fffffff;
  buffer_mutex = xSemaphoreCreateMutex();
  level_mutex = xSemaphoreCreateMutex();
  auto *memory = heap_caps_malloc(sizeof(LogBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory || !buffer_mutex || !level_mutex) abort();
  buffer = new (memory) LogBuffer{};
  apply(LOG_DEFAULT_LEVEL);
  console_output = esp_log_set_vprintf(capture);
}
void logs_load_level() {
  uint8_t level = LOG_DEFAULT_LEVEL;
  size_t size = sizeof(level);
  auto e = app_config_get("logs", 1, &level, &size);
  if (e == ESP_OK && size == sizeof(level) && log_level_valid(level)) apply(level);
  else if (e != ESP_ERR_NVS_NOT_FOUND)
    ESP_LOGW("logs", "Invalid or unreadable saved level; using Warning");
}
int logs_level() { return threshold.load(); }
esp_err_t logs_setting(int level) {
  if (!log_level_valid(level)) return ESP_ERR_INVALID_ARG;
  Guard serial(level_mutex);
  uint8_t value = level;
  auto e = app_config_set("logs", 1, &value, sizeof(value));
  if (e == ESP_OK) { apply(level); publish(SETTINGS); }
  return e;
}
bool logs_snapshot(LogBuffer &out, uint32_t &lost) {
  if (!buffer || xSemaphoreTake(buffer_mutex, 0) != pdTRUE) return false;
  assembler.expire(now_ms(), *buffer);
  out = *buffer;
  lost = dropped.load();
  xSemaphoreGive(buffer_mutex);
  return true;
}
bool logs_clear() {
  if (!buffer || xSemaphoreTake(buffer_mutex, 0) != pdTRUE) return false;
  assembler.clear(); buffer->clear(); dropped = 0;
  xSemaphoreGive(buffer_mutex);
  return true;
}
} // namespace ya
