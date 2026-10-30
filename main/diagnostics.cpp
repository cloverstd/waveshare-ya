#include "data.hpp"
#include "notifications.hpp"
#include "logs.hpp"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include <ctime>
#include <new>
#include "cJSON.h"
#include "driver/usb_serial_jtag.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "bsp/esp-bsp.h"
#include "esp_lv_adapter.h"
#include "ya.hpp"
#include "ota_contract.hpp"
#include "identity.hpp"
#include <fcntl.h>
#include <string>
#include <unistd.h>
namespace ya {
extern std::atomic<int> requested_page;
extern std::atomic<bool> capture_requested;
extern std::atomic<lv_draw_buf_t *> captured;
struct GattSummary {
  int kind, status;
  uint16_t connection, handle, start, end, length;
  char uuid[40];
  uint8_t data[32];
};
static GattSummary *gatt_history;
static unsigned gatt_count = 0;
static bool send_snapshot_line(const char *data, size_t length) {
  flockfile(stdout);
  const int64_t deadline = now_ms() + 2000;
  while (length && now_ms() < deadline) {
    int n = usb_serial_jtag_write_bytes(data, length, pdMS_TO_TICKS(100));
    if (n > 0) {
      data += n;
      length -= n;
    }
  }
  funlockfile(stdout);
  return length == 0;
}
static void send_json(const char *json) {
  // VFS emits one character at a time and may silently drop characters after
  // its TX timeout. Larger status replies need bounded, checked bulk writes.
  // Share stdout's stream lock with normal logging, but never hold GUI locks.
  flockfile(stdout);
  const int64_t deadline = now_ms() + 2000;
  for (const char *part : {"YA_JSON ", json, "\n"}) {
    size_t left = strlen(part);
    while (left && now_ms() < deadline) {
      int sent = usb_serial_jtag_write_bytes(part, left, pdMS_TO_TICKS(100));
      if (sent > 0) {
        part += sent;
        left -= sent;
      }
    }
    if (left)
      break;
  }
  funlockfile(stdout);
}
// Bounded LVGL-state inspection for regression checks. Never copy textareas
// (Wi-Fi credentials) or local OTA access URLs into diagnostic output.
static void inspect_ui(lv_obj_t *obj, char *text, size_t size, int depth = 0) {
  if (depth > 12 || !lv_obj_is_visible(obj) || lv_obj_check_type(obj, &lv_textarea_class))
    return;
  if (lv_obj_check_type(obj, &lv_label_class)) {
    const char *value = lv_label_get_text(obj);
    if (!strstr(value, "http://") && !strstr(value, "https://")) {
      strlcat(text, value, size);
      strlcat(text, "\n", size);
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
    inspect_ui(lv_obj_get_child(obj, i), text, size, depth + 1);
}
static void gatt_handler(void *, esp_event_base_t, int32_t, void *ptr) {
  auto &e = *(GattEvent *)ptr;
  Guard g(state_mutex);
  auto &r = gatt_history[gatt_count++ % 32];
  r = {};
  r.kind = e.kind;
  r.status = e.status;
  r.connection = e.connection;
  r.handle = e.handle;
  r.start = e.start;
  r.end = e.end;
  r.length = std::min<size_t>(e.length, 32);
  strlcpy(r.uuid, e.uuid, sizeof(r.uuid));
  memcpy(r.data, e.data, r.length);
}
static void task(void *) {
  // A reusable PSRAM snapshot avoids reserving several KiB in every diagnostic
  // task frame. The task stack itself stays in internal RAM for NVS/flash calls.
  auto *status_buffer = new Status;
  char line[1024];
  size_t n = 0;
  bool overflow = false;
  for (;;) {
    char c;
    int r = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(20));
    if (r <= 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (c == '\r')
      continue;
    if (c != '\n') {
      if (n < sizeof(line) - 1 && !overflow)
        line[n++] = c;
      else
        overflow = true;
      continue;
    }
    line[n] = 0;
    auto *j = (!overflow && flat_json_object(line, n)) ? cJSON_Parse(line) : nullptr;
    n = 0;
    overflow = false;
    auto *op = j ? cJSON_GetObjectItem(j, "op") : nullptr;
    auto *out = cJSON_CreateObject();
    bool ok = false;
    if (cJSON_IsString(op)) {
      const char *name = op->valuestring;
      if (!strcmp(name, "ui")) {
        auto *text = static_cast<char *>(calloc(1, 2048));
        if (text && esp_lv_adapter_lock(1000) == ESP_OK) {
          inspect_ui(lv_screen_active(), text, 2048);
          esp_lv_adapter_unlock();
          cJSON_AddStringToObject(out, "text", text);
          ok = true;
        }
        free(text);
      } else if (!strcmp(name, "snapshot")) {
        capture_requested = true;
        auto until = now_ms() + 5000;
        lv_draw_buf_t *image = nullptr;
        while (now_ms() < until && !(image = captured.exchange(nullptr)))
          vTaskDelay(pdMS_TO_TICKS(20));
        if (image) {
          char output[300];
          int length = snprintf(output, sizeof output, "YA_IMAGE %u %u %u\n", image->header.w,
                                image->header.h, image->header.stride);
          bool sent = send_snapshot_line(output, length);
          for (size_t offset = 0; sent && offset < image->data_size; offset += 128) {
            size_t n = std::min<size_t>(128, image->data_size - offset);
            int prefix = snprintf(output, sizeof output, "YA_PIX %u ", (unsigned)offset);
            for (size_t i = 0; i < n; ++i)
              sprintf(output + prefix + 2 * i, "%02x", image->data[offset + i]);
            output[prefix + 2 * n] = '\n';
            sent = send_snapshot_line(output, prefix + 2 * n + 1);
            vTaskDelay(1);
          }
          if (sent)
            send_snapshot_line("YA_IMAGE_END\n", 13);
          bsp_display_lock(-1);
          lv_draw_buf_destroy(image);
          bsp_display_unlock();
          ok = true;
        }
      } else if (!strcmp(name, "notifications") || !strcmp(name, "notifications_clear")) {
        if (!strcmp(name,"notifications_clear")) ok = notifications_clear();
        else {
          auto *v = new NotificationLog{};
          notifications_snapshot(*v);
          cJSON_AddNumberToObject(out,"count",v->count);
          cJSON_AddNumberToObject(out,"unread",v->unread());
          auto *items=cJSON_AddArrayToObject(out,"items");
          for(unsigned i=0;i<v->count;++i) {
            auto *e=cJSON_CreateObject();
            cJSON_AddNumberToObject(e,"id",v->entries[i].id);
            cJSON_AddNumberToObject(e,"time",v->entries[i].epoch);
            cJSON_AddStringToObject(e,"text",v->entries[i].text);
            cJSON_AddItemToArray(items,e);
          }
          delete v; ok=true;
        }
      } else if (!strcmp(name, "logs") || !strcmp(name, "logs_clear")) {
        if (!strcmp(name, "logs_clear")) ok = logs_clear();
        else {
          auto *memory = heap_caps_malloc(sizeof(LogBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          auto *v = memory ? new (memory) LogBuffer{} : nullptr;
          uint32_t lost = 0;
          if (v && logs_snapshot(*v, lost)) {
            cJSON_AddNumberToObject(out, "level", logs_level());
            cJSON_AddNumberToObject(out, "count", v->count);
            cJSON_AddNumberToObject(out, "overwritten", v->overwritten);
            cJSON_AddNumberToObject(out, "dropped", lost);
            auto *items = cJSON_AddArrayToObject(out, "items");
            for (unsigned i = 0; i < v->count; ++i) {
              auto *entry = cJSON_CreateObject();
              cJSON_AddNumberToObject(entry, "level", v->at(i).level);
              cJSON_AddStringToObject(entry, "text", v->at(i).text);
              cJSON_AddItemToArray(items, entry);
            }
            ok = true;
          }
          heap_caps_free(v);
        }
      } else if (!strcmp(name, "status")) {
        {
          Guard g(state_mutex);
          *status_buffer = state;
        }
        const auto &s = *status_buffer;
        auto cf = config();
        cJSON_AddStringToObject(out, "version", esp_app_get_description()->version);
        char build_hash[65];
        for (int i = 0; i < 32; i++)
          sprintf(build_hash + 2 * i, "%02x", esp_app_get_description()->app_elf_sha256[i]);
        cJSON_AddStringToObject(out, "build_sha256", build_hash);
        cJSON_AddStringToObject(out, "slot", esp_ota_get_running_partition()->label);
        cJSON_AddStringToObject(out, "ip", s.ip);
        cJSON_AddStringToObject(out, "wifi", s.wifi_note);
        cJSON_AddStringToObject(out, "ota", s.ota_note);
        cJSON_AddBoolToObject(out, "pending", s.pending);
        cJSON_AddNumberToObject(out, "ota_progress", s.ota_progress);
        cJSON_AddStringToObject(out, "ota_phase", ota_phase_name(s.ota_phase));
        cJSON_AddStringToObject(out, "ota_mode", s.ota_mode==OtaMode::DELTA ? "delta" : "full");
        cJSON_AddNumberToObject(out, "ota_received", s.ota_received);
        cJSON_AddNumberToObject(out, "ota_total", s.ota_total);
        cJSON_AddNumberToObject(out, "ota_written", s.ota_written);
        cJSON_AddBoolToObject(out, "delta_available", s.ota_delta_available);
        cJSON_AddNumberToObject(out, "full_size", s.ota_full_size);
        cJSON_AddNumberToObject(out, "delta_size", s.ota_delta_size);
        cJSON_AddStringToObject(out, "target_sha256", s.ota_sha);
        cJSON_AddBoolToObject(out, "battery_valid", s.battery.valid);
        cJSON_AddNumberToObject(out, "battery", s.battery.percent);
        cJSON_AddNumberToObject(out, "battery_mv", s.battery.millivolts);
        cJSON_AddBoolToObject(out, "usb", s.battery.usb);
        cJSON_AddBoolToObject(out, "charging", s.battery.charging);
        cJSON_AddNumberToObject(out, "temperature", s.battery.temperature);
        cJSON_AddBoolToObject(out, "sd", s.sd_mounted);
        cJSON_AddNumberToObject(out, "brightness", cf.brightness);
        cJSON_AddNumberToObject(out, "fps", cf.fps);
        cJSON_AddNumberToObject(out, "volume", cf.volume);
        cJSON_AddBoolToObject(out, "mute", cf.mute);
        cJSON_AddNumberToObject(out, "audio_mute_applied", board_audio_mute_state());
        cJSON_AddNumberToObject(out, "heap", esp_get_free_heap_size());
        cJSON_AddNumberToObject(out, "internal_heap", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "internal_largest",
                                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        cJSON_AddNumberToObject(out, "epoch", time(nullptr));
        cJSON_AddBoolToObject(out, "ble_enabled", cf.ble);
        cJSON_AddStringToObject(out,"ble_name",ble_name());
        cJSON_AddBoolToObject(out, "ble_services_valid", s.ble_services_valid);
        cJSON_AddBoolToObject(out, "phone_connected", s.phone_connected);
        cJSON_AddNumberToObject(out, "ble_disconnect_reason", s.ble_disconnect_reason);
        cJSON_AddStringToObject(out,"ble_drop_note",s.ble_drop_note);
        cJSON_AddStringToObject(out,"data_route",data_diagnostics().c_str());
        auto *links = cJSON_AddArrayToObject(out, "ble_links");
        for (const auto &link : s.gateways) if (link.handle >= 0) {
          auto *v = cJSON_CreateObject();
          cJSON_AddNumberToObject(v, "handle", link.handle);
          cJSON_AddBoolToObject(v, "encrypted", link.encrypted);
          cJSON_AddBoolToObject(v, "authenticated", link.authenticated);
          cJSON_AddBoolToObject(v, "subscribed", link.subscribed);
          cJSON_AddBoolToObject(v, "ready", link.ready);
          cJSON_AddItemToArray(links, v);
        }
        cJSON_AddBoolToObject(out, "wifi_enabled", cf.wifi);
        auto prefs = system_prefs();
        cJSON_AddNumberToObject(out, "sleep", effective_sleep(s.battery));
        cJSON_AddNumberToObject(out, "sleep_battery", prefs.sleep_battery);
        cJSON_AddNumberToObject(out, "sleep_usb", prefs.sleep_usb);
        cJSON_AddNumberToObject(out, "log_level", logs_level());
        cJSON_AddStringToObject(out, "wifi_ssid", s.wifi_ssid);
        cJSON_AddNumberToObject(out, "wifi_rssi", s.wifi_rssi);
        cJSON_AddBoolToObject(out, "wifi_connected", s.wifi_connected);
        cJSON_AddStringToObject(out, "notice", s.notice);
        cJSON_AddNumberToObject(out, "notice_expires", s.notice_expires);
        cJSON_AddBoolToObject(out, "time_valid", s.time_valid);
        cJSON_AddBoolToObject(out, "rtc_valid", s.rtc_valid);
        cJSON_AddBoolToObject(out, "time_syncing", s.time_syncing);
        cJSON_AddStringToObject(out, "time_note", s.time_note);
        cJSON_AddNumberToObject(out, "last_time_sync", s.last_time_sync);
        cJSON_AddNumberToObject(out, "ram_free", s.ram_free);
        cJSON_AddNumberToObject(out, "ram_total", s.ram_total);
        cJSON_AddNumberToObject(out, "psram_free", s.psram_free);
        cJSON_AddNumberToObject(out, "psram_total", s.psram_total);
        cJSON_AddNumberToObject(out, "internal_min", s.internal_min);
        cJSON_AddBoolToObject(out, "flash_mounted", s.flash_mounted);
        cJSON_AddBoolToObject(out, "flash_capacity_valid", s.flash_capacity_valid);
        cJSON_AddBoolToObject(out, "sd_capacity_valid", s.sd_capacity_valid);
        cJSON_AddNumberToObject(out, "flash_total", s.flash_total);
        cJSON_AddNumberToObject(out, "flash_free", s.flash_free);
        cJSON_AddNumberToObject(out, "sd_total", s.sd_total);
        cJSON_AddNumberToObject(out, "sd_free", s.sd_free);
        cJSON_AddStringToObject(out, "ota_phase", ota_phase_name(s.ota_phase));
        cJSON_AddNumberToObject(out, "ota_received", s.ota_received);
        cJSON_AddNumberToObject(out, "ota_total", s.ota_total);
        auto metrics = display_metrics_snapshot();
        cJSON_AddNumberToObject(out, "actual_fps", metrics.actual_fps);
        cJSON_AddNumberToObject(out, "refresh_count", metrics.refresh_count);
        cJSON_AddNumberToObject(out, "flush_chunks", metrics.flush_chunks);
        cJSON_AddNumberToObject(out, "flushed_pixels", metrics.flushed_pixels);
        cJSON_AddNumberToObject(out, "refresh_work_total_us", metrics.refresh_work_total_us);
        cJSON_AddNumberToObject(out, "flush_submit_total_us", metrics.flush_submit_total_us);
        cJSON_AddNumberToObject(out, "frame_interval_samples", metrics.frame_interval_samples);
        cJSON_AddNumberToObject(out, "long_gap_count", metrics.long_gap_count);
        cJSON_AddNumberToObject(out, "max_gap_us", metrics.max_gap_us);
        cJSON_AddNumberToObject(out, "refresh_p50_us", metrics.refresh_p50_us);
        cJSON_AddNumberToObject(out, "refresh_p95_us", metrics.refresh_p95_us);
        cJSON_AddNumberToObject(out, "refresh_work_p95_us", metrics.refresh_work_p95_us);
        cJSON_AddNumberToObject(out, "flush_wait_p95_us", metrics.flush_wait_p95_us);
        cJSON_AddNumberToObject(out, "uptime_ms", now_ms());
        cJSON_AddNumberToObject(out, "main_stack_free_min_bytes",
                                main_task_handle ? uxTaskGetStackHighWaterMark(main_task_handle)
                                                 : 0);
        cJSON_AddNumberToObject(out, "diag_stack_free_min_bytes",
                                uxTaskGetStackHighWaterMark(nullptr));
        cJSON_AddNumberToObject(out, "reset_reason", esp_reset_reason());
        ok = true;
      } else if (!strcmp(name, "wifi_state")) {
        {
          Guard g(state_mutex);
          *status_buffer = state;
        }
        const auto &s = status_buffer->wlan;
        cJSON_AddNumberToObject(out, "phase", int(s.phase));
        cJSON_AddNumberToObject(out, "current_id", s.current_id);
        cJSON_AddNumberToObject(out, "scan_version", s.scan_version);
        cJSON_AddBoolToObject(out, "k", s.k);
        cJSON_AddBoolToObject(out, "v", s.v);
        auto *near = cJSON_AddArrayToObject(out, "nearby");
        for (int i = 0; i < s.nearby_count; ++i) {
          auto &n = s.nearby[i];
          auto *v = cJSON_CreateObject();
          cJSON_AddStringToObject(v, "ssid", n.ssid);
          cJSON_AddNumberToObject(v, "security", int(n.security));
          cJSON_AddNumberToObject(v, "rssi", n.rssi);
          cJSON_AddItemToArray(near, v);
        }
        auto *saved = cJSON_AddArrayToObject(out, "saved");
        for (int i = 0; i < s.saved_count; ++i) {
          auto &n = s.saved[i];
          auto *v = cJSON_CreateObject();
          cJSON_AddStringToObject(v, "ssid", n.ssid);
          cJSON_AddNumberToObject(v, "id", n.id);
          cJSON_AddNumberToObject(v, "security", int(n.security));
          cJSON_AddBoolToObject(v, "automatic", n.automatic);
          cJSON_AddBoolToObject(v, "auth_failed", n.auth_failed);
          cJSON_AddItemToArray(saved, v);
        }
        ok = true;
      } else if (!strcmp(name, "wifi_view")) {
        auto *v = cJSON_GetObjectItem(j, "view"), *id = cJSON_GetObjectItem(j, "id");
        if (cJSON_IsNumber(v) && cJSON_IsNumber(id) && v->valueint >= 1 && v->valueint <= 3) {
          wifi_ui_preview(v->valueint, id->valueint);
          ok = true;
        }
      } else if (!strcmp(name, "wifi_scan")) {
        ok = command(Op::WIFI_SCAN);
      } else if (!strcmp(name, "wifi_profile")) {
        auto *a = cJSON_GetObjectItem(j, "action"), *id = cJSON_GetObjectItem(j, "id");
        if (cJSON_IsString(a) && cJSON_IsNumber(id) && id->valueint > 0 && !ota_busy) {
          if (!strcmp(a->valuestring, "connect"))
            ok = command(Op::WIFI_SAVED_CONNECT, "", "", id->valueint);
          else if (!strcmp(a->valuestring, "forget"))
            ok = command(Op::WIFI_PROFILE_FORGET, "", "", id->valueint);
          else if (!strcmp(a->valuestring, "auto")) {
            auto *v = cJSON_GetObjectItem(j, "enabled");
            if (cJSON_IsBool(v))
              ok = command(Op::WIFI_PROFILE_AUTO, cJSON_IsTrue(v) ? "1" : "0", "", id->valueint);
          }
        }
      } else if (!strcmp(name, "wifi")) {
        auto *a = cJSON_GetObjectItem(j, "ssid"), *b = cJSON_GetObjectItem(j, "password");
        ok = cJSON_IsString(a) && cJSON_IsString(b) && !ota_busy &&
             command(Op::WIFI_CONNECT, a->valuestring, b->valuestring,
                     cJSON_GetObjectItem(j, "options") ? cJSON_GetObjectItem(j, "options")->valueint
                                                       : 0);
      } else if (!strcmp(name, "set")) {
        auto *k = cJSON_GetObjectItem(j, "key"), *v = cJSON_GetObjectItem(j, "value");
        ok = cJSON_IsString(k) && cJSON_IsNumber(v) &&
             setting(k->valuestring, v->valueint) == ESP_OK;
        if (cJSON_IsString(k) && !strcmp(k->valuestring, "sleep"))
          cJSON_AddStringToObject(out, "error", "use_sleep_battery_or_sleep_usb");
      } else if (!strcmp(name, "page")) {
        auto *p = cJSON_GetObjectItem(j, "id");
        if (cJSON_IsNumber(p) && p->valueint >= 0 && p->valueint <= 14 && p->valueint != 11) {
          requested_page = p->valueint;
          ok = true;
        }
      } else if (!strcmp(name, "scroll")) {
        auto *y = cJSON_GetObjectItem(j, "y");
        if (cJSON_IsNumber(y) && y->valueint >= 0 && y->valueint <= 10000) {
          requested_scroll = y->valueint;
          ok = true;
        }
      } else if (!strcmp(name, "metrics_reset")) {
        display_metrics_reset();
        ok = true;
      } else if (!strcmp(name, "time_sync")) {
        ok = command(Op::TIME_SYNC);
      } else if (!strcmp(name, "ble_scan")) {
        ok = command(Op::BLE_SCAN);
      } else if (!strcmp(name, "ble_peers")) {
        {
          Guard g(state_mutex);
          *status_buffer = ya::state;
        }
        const auto &state = *status_buffer;
        auto *peers = cJSON_AddArrayToObject(out, "peers");
        for (int i = 0; i < state.peer_count; i++) {
          auto *item = cJSON_CreateObject();
          cJSON_AddNumberToObject(item, "index", i);
          cJSON_AddStringToObject(item, "name", state.peers[i].name);
          cJSON_AddNumberToObject(item, "handle", state.peers[i].handle);
          cJSON_AddBoolToObject(item, "connected", state.peers[i].connected);
          cJSON_AddItemToArray(peers, item);
        }
        ok = true;
      } else if (!strcmp(name, "ble_connect") || !strcmp(name, "ble_disconnect")) {
        auto *index = cJSON_GetObjectItem(j, "index");
        ok = cJSON_IsNumber(index) &&
             command(!strcmp(name, "ble_connect") ? Op::BLE_CONNECT : Op::BLE_DISCONNECT, "", "",
                     index->valueint);
      } else if (!strcmp(name, "ble_read") || !strcmp(name, "ble_subscribe")) {
        auto *conn = cJSON_GetObjectItem(j, "connection");
        auto *handle = cJSON_GetObjectItem(j, "handle");
        if (cJSON_IsNumber(conn) && cJSON_IsNumber(handle) && conn->valueint >= 0 &&
            conn->valueint <= 65535 && handle->valueint > 0 && handle->valueint <= 65535)
          ok = (!strcmp(name, "ble_read")
                    ? ble_read(conn->valueint, handle->valueint)
                    : ble_subscribe(conn->valueint, handle->valueint)) == ESP_OK;
      } else if (!strcmp(name, "gatt_history")) {
        auto *events = cJSON_AddArrayToObject(out, "events");
        Guard g(state_mutex);
        for (unsigned i = gatt_count > 32 ? gatt_count - 32 : 0; i < gatt_count; i++) {
          auto &e = gatt_history[i % 32];
          auto *item = cJSON_CreateObject();
          cJSON_AddNumberToObject(item, "kind", e.kind);
          cJSON_AddNumberToObject(item, "status", e.status);
          cJSON_AddNumberToObject(item, "connection", e.connection);
          cJSON_AddNumberToObject(item, "handle", e.handle);
          cJSON_AddStringToObject(item, "uuid", e.uuid);
          char hex[65];
          for (int k = 0; k < e.length; k++)
            sprintf(hex + 2 * k, "%02x", e.data[k]);
          hex[e.length * 2] = 0;
          cJSON_AddStringToObject(item, "data", hex);
          cJSON_AddItemToArray(events, item);
        }
        ok = true;
      } else if (!strcmp(name, "ota_session")) {
        ok = command(Op::OTA_SESSION);
      } else if (!strcmp(name, "ota_url")) {
        {
          Guard g(state_mutex);
          *status_buffer = state;
        }
        const auto &s = *status_buffer;
        cJSON_AddStringToObject(out, "url", s.ota_url);
        ok = true;
      } else if (!strcmp(name, "update_url")) {
        auto *u = cJSON_GetObjectItem(j, "url");
        ok = cJSON_IsString(u) && set_update_url(u->valuestring) == ESP_OK;
      } else if (!strcmp(name, "check"))
        ok = command(Op::OTA_CHECK);
      else if (!strcmp(name, "install")) {
        auto *mode=cJSON_GetObjectItemCaseSensitive(j,"mode");
        auto *sha=cJSON_GetObjectItemCaseSensitive(j,"sha256");
        bool valid=!mode || (cJSON_IsString(mode) &&
            (!strcmp(mode->valuestring,"full") || !strcmp(mode->valuestring,"delta")));
        char target[65];
        { Guard g(state_mutex); strlcpy(target,state.ota_sha,sizeof(target)); }
        if (sha) valid=valid && cJSON_IsString(sha) && ota_contract::digest(sha->valuestring);
        if (valid) ok=command(Op::OTA_INSTALL,"",sha ? sha->valuestring : target,
            mode && !strcmp(mode->valuestring,"delta") ? (int)OtaMode::DELTA : (int)OtaMode::FULL);
      } else if (!strcmp(name, "reboot"))
        ok = command(Op::REBOOT);
      else if (!strcmp(name, "audio"))
        ok = command(Op::AUDIO_TEST);
      else if (!strcmp(name, "sd_mount"))
        ok = command(Op::SD_MOUNT);
      else if (!strcmp(name, "ble_pair"))
        ok = command(Op::BLE_PAIR);
    }
    auto *request_id = j ? cJSON_GetObjectItem(j, "seq") : nullptr;
    if (cJSON_IsNumber(request_id))
      cJSON_AddNumberToObject(out, "seq", request_id->valuedouble);
    cJSON_AddBoolToObject(out, "ok", ok);
    char *s = cJSON_PrintUnformatted(out);
    if (s) {
      send_json(s);
      free(s);
    }
    cJSON_Delete(out);
    cJSON_Delete(j);
    memset(line, 0, sizeof(line));
  }
}
void diagnostics_init() {
  gatt_history = (GattSummary *)calloc(32, sizeof(GattSummary));
  if (!gatt_history)
    abort();
  ESP_ERROR_CHECK(esp_event_handler_register(EVENTS, GATT, gatt_handler, nullptr));
  usb_serial_jtag_driver_config_t c{};
  c.rx_buffer_size = 2048;
  c.tx_buffer_size = 4096;
  auto result = usb_serial_jtag_driver_install(&c);
  if (result == ESP_OK || result == ESP_ERR_INVALID_STATE) {
    usb_serial_jtag_vfs_use_driver();
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    static StackType_t stack[8192];
    static StaticTask_t control;
    if (!xTaskCreateStatic(task, "usb_diag", sizeof stack, nullptr, 3, stack, &control))
      abort();
  }
}
} // namespace ya
