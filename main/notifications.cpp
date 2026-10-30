#include "notifications.hpp"
#include "ya.hpp"
#include "nvs.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include <ctime>
#include <new>
namespace ya {
static SemaphoreHandle_t mutex, store_mutex;
static NotificationLog *logbook;
static uint32_t boot_id, saved_revision = 0, clear_through = 0;
static bool clearing = false;
static std::atomic<int> clear_result{0};
static int64_t last_save = 0;
static bool valid(const NotificationLog &v) {
  if (v.schema != 1 || v.count > 10 || !v.next) return false;
  uint32_t previous = 0;
  for (unsigned i = 0; i < v.count; ++i) {
    const auto &e = v.entries[i];
    if (e.id <= previous || e.id >= v.next || !memchr(e.text, 0, sizeof(e.text)) ||
        static_cast<unsigned>(e.kind) > 3) return false;
    previous = e.id;
  }
  return true;
}
void notifications_init() {
  mutex = xSemaphoreCreateMutex(); store_mutex = xSemaphoreCreateMutex();
  logbook = new NotificationLog{};
  if (!mutex || !store_mutex || !logbook) abort();
  boot_id = esp_random();
  nvs_handle_t h;
  if (nvs_open("notifications", NVS_READONLY, &h) == ESP_OK) {
    size_t n = sizeof(*logbook);
    auto e = nvs_get_blob(h, "history", logbook, &n);
    if (e != ESP_OK || n != sizeof(*logbook) || !valid(*logbook)) new (logbook) NotificationLog{};
    nvs_close(h);
  }
  saved_revision = logbook->revision;
}
void notifications_add(const char *text, NoticeKind kind, bool toast) {
  if (!logbook || !text || !*text) return;
  const auto t = time(nullptr);
  { Guard g(mutex); logbook->append(text, kind, boot_id, now_ms(), t > 1700000000 ? t : 0); }
  if (toast) {
    Guard g(state_mutex);
    strlcpy(state.notice, text, sizeof(state.notice));
    state.notice_expires = now_ms() + 5000;
  }
}
void notifications_snapshot(NotificationLog &v) { Guard g(mutex); v = *logbook; }
void notifications_read(uint32_t through) {
  Guard g(mutex);
  if (through > logbook->read) { logbook->read = through; ++logbook->revision; }
}
bool notifications_clear() {
  Guard g(mutex);
  if (clearing) return false;
  clear_through = logbook->next - 1; clearing = true; clear_result = 0; return true;
}
int notifications_clear_result() { return clear_result.exchange(0); }
bool notifications_checkpoint() {
  if (!logbook) return true;
  Guard storage(store_mutex);
  auto *copy = new NotificationLog{};
  if (!copy) return false;
  bool clear; uint32_t cutoff, revision;
  {
    Guard g(mutex);
    clear = clearing; cutoff = clear_through; revision = logbook->revision;
    if (!clear && revision == saved_revision) { delete copy; return true; }
    *copy = *logbook;
  }
  if (clear) copy->erase_through(cutoff);
  nvs_handle_t h;
  esp_err_t e = nvs_open("notifications", NVS_READWRITE, &h);
  if (e == ESP_OK) {
    e = nvs_set_blob(h, "history", copy, sizeof(*copy));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
  }
  {
    Guard g(mutex);
    if (e == ESP_OK) {
      saved_revision = revision;
      if (clear) { logbook->erase_through(cutoff); saved_revision = revision + 1; }
    }
    if (clear) { clearing = false; clear_result = e == ESP_OK ? 1 : -1; }
  }
  delete copy; return e == ESP_OK;
}
void notifications_tick() {
  auto t = time(nullptr);
  if (t > 1700000000) { Guard g(mutex); logbook->correct_time(boot_id, now_ms(), t); }
  if (now_ms() - last_save >= 1000) { last_save = now_ms(); notifications_checkpoint(); }
}
}
