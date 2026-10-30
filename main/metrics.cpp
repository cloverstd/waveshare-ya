#include "ya.hpp"
#include "esp_lv_adapter.h"
#include <algorithm>
#include <array>

namespace ya {
namespace {
constexpr size_t SAMPLES = 128;
portMUX_TYPE metric_lock = portMUX_INITIALIZER_UNLOCKED;
DisplayMetrics counters;
std::array<uint32_t, SAMPLES> intervals{}, work{}, waits{};
size_t used = 0, cursor = 0, interval_used = 0, interval_cursor = 0;
int64_t started = 0, last_frame = 0, wait_started = 0, flush_started = 0;
uint32_t frame_wait = 0;
bool dirty = false;
std::atomic<bool> reset_pending{false};

void event(lv_event_t *e) {
  const auto code = lv_event_get_code(e);
  const int64_t now = esp_timer_get_time();
  bool resetting = code == LV_EVENT_REFR_START && reset_pending.exchange(false);
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
  if (resetting)
    esp_lv_adapter_fps_stats_reset(nullptr);
#endif
  portENTER_CRITICAL(&metric_lock);
  if (code == LV_EVENT_REFR_START) {
    if (resetting) {
      counters = {};
      used = cursor = interval_used = interval_cursor = 0;
      last_frame = 0;
    }
    started = now;
    dirty = false;
    frame_wait = 0;
  } else if (code == LV_EVENT_FLUSH_START) {
    dirty = true;
    flush_started = now;
    counters.flush_chunks++;
    auto *area = static_cast<const lv_area_t *>(lv_event_get_param(e));
    if (area)
      counters.flushed_pixels += lv_area_get_size(area);
  } else if (code == LV_EVENT_FLUSH_FINISH) {
    if (flush_started)
      counters.flush_submit_total_us += now - flush_started;
    flush_started = 0;
  } else if (code == LV_EVENT_FLUSH_WAIT_START) {
    wait_started = now;
  } else if (code == LV_EVENT_FLUSH_WAIT_FINISH) {
    if (wait_started)
      frame_wait += uint32_t(now - wait_started);
    wait_started = 0;
  } else if (code == LV_EVENT_REFR_READY && dirty) {
    counters.refresh_count++;
    counters.refresh_work_total_us += now - started;
    work[cursor] = uint32_t(now - started);
    waits[cursor] = frame_wait;
    cursor = (cursor + 1) % SAMPLES;
    used = std::min(used + 1, SAMPLES);
    // Only contiguous motion frames: idle gaps are not rendering stalls.
    if (last_frame) {
      const uint32_t gap = uint32_t(std::min<int64_t>(now - last_frame, UINT32_MAX));
      counters.max_gap_us = std::max(counters.max_gap_us, gap);
      if (gap > 500000)
        counters.long_gap_count++;
    }
    if (last_frame && now - last_frame <= 500000) {
      intervals[interval_cursor] = uint32_t(now - last_frame);
      interval_cursor = (interval_cursor + 1) % SAMPLES;
      interval_used = std::min(interval_used + 1, SAMPLES);
    }
    last_frame = now;
  }
  portEXIT_CRITICAL(&metric_lock);
}
uint32_t percentile(std::array<uint32_t, SAMPLES> &a, size_t n, size_t p) {
  if (!n)
    return 0;
  std::sort(a.begin(), a.begin() + n);
  return a[((n - 1) * p + 99) / 100];
}
} // namespace

void display_metrics_init() {
  auto *display = lv_display_get_default();
  for (auto code : {LV_EVENT_REFR_START, LV_EVENT_REFR_READY, LV_EVENT_FLUSH_START,
                    LV_EVENT_FLUSH_FINISH, LV_EVENT_FLUSH_WAIT_START, LV_EVENT_FLUSH_WAIT_FINISH})
    lv_display_add_event_cb(display, event, code, nullptr);
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
  esp_lv_adapter_fps_stats_enable(display, true);
#endif
}

DisplayMetrics display_metrics_snapshot() {
  std::array<uint32_t, SAMPLES> a, b, c;
  size_t n, count;
  int64_t last;
  DisplayMetrics out;
  portENTER_CRITICAL(&metric_lock);
  out = counters;
  a = intervals;
  b = work;
  c = waits;
  n = used;
  count = interval_used;
  last = last_frame;
  portEXIT_CRITICAL(&metric_lock);
  out.frame_interval_samples = count;
  out.refresh_p50_us = percentile(a, count, 50);
  out.refresh_p95_us = percentile(a, count, 95);
  out.refresh_work_p95_us = percentile(b, n, 95);
  out.flush_wait_p95_us = percentile(c, n, 95);
#if CONFIG_ESP_LVGL_ADAPTER_ENABLE_FPS_STATS
  // Adapter's counter advances on final DMA completion; it otherwise retains
  // an old FPS while idle. Do not report that stale value as current FPS.
  if (last && esp_timer_get_time() - last < 1500000)
    esp_lv_adapter_get_fps(nullptr, &out.actual_fps);
#endif
  return out;
}
void display_metrics_reset() { reset_pending = true; }
} // namespace ya
