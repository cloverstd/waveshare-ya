#include "notifications.hpp"
#include "logs.hpp"
#include "esp_err.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "ya.hpp"
#include "power.hpp"
#include "ui_policy.hpp"
#include <ctime>
#include <string>
#include <algorithm>
#include <new>
#include "esp_heap_caps.h"
namespace ya {
std::atomic<int> requested_page{-1};
std::atomic<int> requested_scroll{-1};
std::atomic<bool> capture_requested{false};
std::atomic<lv_draw_buf_t *> captured{nullptr};
static lv_obj_t *root, *content, *headline, *clock_label, *notification, *wifi_icon, *wifi_slash,
    *bluetooth_icon, *bluetooth_slash, *status_icons, *charging_icon, *battery_icon, *battery_tip, *battery_number,
    *ota_bar = nullptr, *ota_status = nullptr, *ota_bytes = nullptr, *detail = nullptr,
    *list = nullptr, *modal = nullptr, *wake_shield = nullptr;
static lv_obj_t *ssid_input, *pass_input, *text_input;
static const App *active = nullptr;
static bool asleep = false;
static int64_t activity = 0;
static uint8_t applied_brightness = 0, applied_fps = 0;
static int page_id = 0, list_count = -1;
static uint32_t generation = 0, modal_generation = 0, url_request = 0;
static int brightness_preview = -1;
static uint32_t brightness_request = 0;
static bool keep_awake = false, last_usb = false;
static int64_t seen_notice_expires = 0;
static lv_obj_t *notice_badge = nullptr, *notice_list = nullptr, *clear_button = nullptr;
static lv_obj_t *sys_values[8]{}, *sys_bars[4]{}, *battery_arc = nullptr;
static lv_obj_t *ble_summary = nullptr, *ble_code = nullptr, *ble_pair_button = nullptr;
static lv_obj_t *ota_full_button=nullptr, *ota_delta_button=nullptr, *ota_install_button=nullptr,
    *ota_check_button=nullptr, *ota_options=nullptr;
static OtaMode ota_selected=OtaMode::FULL, ota_confirmed=OtaMode::FULL;
static char ota_confirm_sha[65]{};
static uint32_t ota_choice_revision=~0u;
static uint32_t notice_revision = ~0u;
static bool clearing_notices = false;
static lv_obj_t *log_list = nullptr, *log_summary = nullptr, *log_pause_button = nullptr;
static LogBuffer *log_view = nullptr;
static bool log_paused = false, log_rendering = false;
static uint32_t log_revision = ~0u, log_dropped = ~0u;
static int64_t log_refresh_at = 0;
static void logs_refresh();
static const NotificationLog &notice_snapshot() {
  static auto *v = new NotificationLog{};
  notifications_snapshot(*v); return *v;
}
static void system_refresh(const Status &);
static void notices_refresh();
static void notice_badge_refresh();
enum class ControlType { SLIDER, TOGGLE, SELECT };
struct Control {
  lv_obj_t *widget = nullptr, *value_label = nullptr;
  const char *key = nullptr, *title = nullptr;
  ControlType type = ControlType::TOGGLE;
  uint32_t request = 0;
  bool dragging = false;
};
static Control controls[20];
static size_t control_count = 0;
struct Pending {
  uint32_t id = 0, page_generation = 0, dialog_generation = 0;
  bool url = false;
};
static Pending pending[12];
static std::atomic<int> wifi_preview_view{0}, wifi_preview_id{0};
void wifi_ui_preview(int view, int id) {
  wifi_preview_id = id;
  wifi_preview_view = view;
}
static uint32_t wifi_forget_pending_id = 0;
static lv_obj_t *wifi_bars[4]{};
static lv_obj_t *wifi_current = nullptr, *wifi_message = nullptr, *wifi_scan_button = nullptr;
static uint64_t wifi_render_key = ~uint64_t(0);
static int64_t wifi_refresh_at = 0;
struct WifiBinding {
  char ssid[33]{};
  WifiSecurity security = WifiSecurity::OPEN;
  uint32_t id = 0;
};
static WifiBinding *wifi_bindings = nullptr;
// LVGL runs on one task. Reuse a PSRAM snapshot rather than stacking large
// Status copies when refresh opens a dialog or dispatches a page event.
static const Status &ui_snapshot() {
  static Status *buffer = nullptr;
  if (!buffer) {
    buffer = (Status *)heap_caps_malloc(sizeof(Status), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer)
      abort();
    new (buffer) Status{};
  }
  Guard g(state_mutex);
  *buffer = state;
  return *buffer;
}

static WifiBinding wifi_edit;
static uint32_t wifi_replace_id = 0;
static bool wifi_hidden = false;
static lv_obj_t *wifi_security_select = nullptr;

static lv_color_t accent() { return lv_color_hex(0x79d5c8); }
static void safe_area_cover_check(lv_event_t *e) {
  auto *obj = (lv_obj_t *)lv_event_get_current_target(e);
  auto *area = lv_event_get_cover_area(e);
  if (!area || !lv_obj_get_style_clip_corner(obj, LV_PART_MAIN) ||
      lv_obj_get_style_bg_opa(obj, LV_PART_MAIN) != LV_OPA_COVER ||
      lv_obj_get_style_opa(obj, LV_PART_MAIN) != LV_OPA_COVER ||
      lv_obj_get_style_opa_layered(obj, LV_PART_MAIN) != LV_OPA_COVER ||
      lv_obj_get_style_bg_grad_dir(obj, LV_PART_MAIN) != LV_GRAD_DIR_NONE ||
      lv_obj_get_style_bg_grad(obj, LV_PART_MAIN) != nullptr ||
      lv_obj_get_style_bitmap_mask_src(obj, LV_PART_MAIN) != nullptr ||
      lv_obj_get_style_blend_mode(obj, LV_PART_MAIN) != LV_BLEND_MODE_NORMAL ||
      lv_obj_get_transform(obj) != nullptr ||
      lv_obj_get_style_transform_rotation(obj, LV_PART_MAIN) != 0 ||
      lv_obj_get_style_transform_scale_x(obj, LV_PART_MAIN) != 256 ||
      lv_obj_get_style_transform_scale_y(obj, LV_PART_MAIN) != 256 ||
      lv_obj_get_style_transform_skew_x(obj, LV_PART_MAIN) != 0 ||
      lv_obj_get_style_transform_skew_y(obj, LV_PART_MAIN) != 0 ||
      lv_obj_get_style_radius(obj, LV_PART_MAIN) != 64 ||
      lv_obj_get_style_transform_width(obj, LV_PART_MAIN) != 0 ||
      lv_obj_get_style_transform_height(obj, LV_PART_MAIN) != 0)
    return;
  lv_area_t bounds;
  lv_obj_get_coords(obj, &bounds);
  if (bounds.x1 != 0 || bounds.y1 != 0 || bounds.x2 != 479 || bounds.y2 != 479)
    return;
  // LVGL's default COVER_CHECK marks every clip_corner object MASKED. That
  // prevents its opaque content from being selected as the redraw starting
  // point, even when the entire dirty strip is far inside the rounded edge.
  // Our inclusive safe rectangle is strictly within the radius-64 shape:
  // the top-left corner (24,24) is sqrt(40^2 + 40^2) < 64 from (64,64).
  // The other corners are symmetric. Only this fixed, untransformed, opaque
  // interior can bypass that conservative result. All outer areas retain the
  // original MASKED result and the real rounded child clipping remains on.
  if (area->x1 >= 24 && area->y1 >= 24 && area->x2 <= 455 && area->y2 <= 455) {
    // COVER_CHECK begins with COVER. Keep that result using public APIs and
    // stop the class handler before it unconditionally changes it to MASKED.
    // Existing stronger results are never weakened by lv_event_set_cover_res.
    lv_event_set_cover_res(e, LV_COVER_RES_COVER);
    lv_event_stop_processing(e);
  }
}
static void set_text(lv_obj_t *obj, const char *text) {
  if (obj && strcmp(lv_label_get_text(obj), text))
    lv_label_set_text(obj, text);
}
static void close_modal();
static void wake();
static void sleep_screen();
static Pending *reserve_pending() {
  for (auto &p : pending)
    if (!p.id)
      return &p;
  return nullptr;
}
static uint32_t submit_setting(const char *key, int value) {
  auto *p = reserve_pending();
  uint32_t id = p ? setting_async(key, value) : 0;
  if (id)
    *p = {id, generation, modal_generation, false};
  else
    notice("操作队列已满，请重试");
  return id;
}
static int control_value(const char *key, const Config &c, const SystemPrefs &prefs) {
  if (!strcmp(key, "log_level"))
    return logs_level();
  if (!strncmp(key, "power_", 6))
    return power_value(key);
  if (!strcmp(key, "brightness"))
    return c.brightness;
  if (!strcmp(key, "volume"))
    return c.volume;
  if (!strcmp(key, "fps"))
    return c.fps;
  if (!strcmp(key, "wifi"))
    return c.wifi;
  if (!strcmp(key, "ble"))
    return c.ble;
  if (!strcmp(key, "sleep_battery"))
    return prefs.sleep_battery;
  if (!strcmp(key, "sleep_usb"))
    return prefs.sleep_usb;
  return c.mute;
}
static int selected_index(const char *key, int value) {
  if (!strcmp(key, "log_level"))
    return value;
  if (!strncmp(key, "power_", 6)) {
    if (strstr(key, "mode"))
      return value;
    const int minutes[] = {0, 1, 5, 10, 15, 30, 60};
    for (int i = 0; i < 7; i++)
      if (minutes[i] == value)
        return i;
    return 4;
  }
  if (!strcmp(key, "fps"))
    return value == 15 ? 0 : value == 30 ? 1 : 2;
  if (!strcmp(key, "sleep_battery"))
    return value == 30 ? 0 : value == 60 ? 1 : 2;
  return value == 60 ? 0 : value == 300 ? 1 : value == 600 ? 2 : 3;
}
static void sync_controls(const Config &c, const SystemPrefs &prefs) {
  for (size_t i = 0; i < control_count; ++i) {
    auto &ctrl = controls[i];
    if (ctrl.request || ctrl.dragging)
      continue;
    int value = control_value(ctrl.key, c, prefs);
    if (ctrl.type == ControlType::SLIDER) {
      if (lv_slider_get_value(ctrl.widget) != value)
        lv_slider_set_value(ctrl.widget, value, LV_ANIM_OFF);
      char text[80];
      snprintf(text, sizeof(text), "%s  %d%%", ctrl.title, value);
      set_text(ctrl.value_label, text);
    } else if (ctrl.type == ControlType::TOGGLE) {
      bool on = lv_obj_has_state(ctrl.widget, LV_STATE_CHECKED);
      if (on != bool(value)) {
        if (value)
          lv_obj_add_state(ctrl.widget, LV_STATE_CHECKED);
        else
          lv_obj_remove_state(ctrl.widget, LV_STATE_CHECKED);
      }
    } else {
      int idx = selected_index(ctrl.key, value);
      if ((int)lv_dropdown_get_selected(ctrl.widget) != idx)
        lv_dropdown_set_selected(ctrl.widget, idx);
    }
  }
}
static void completions() {
  SettingResult result{};
  while (setting_result(result)) {
    if (result.id == brightness_request) {
      brightness_request = 0;
      brightness_preview = -1;
    }
    for (auto &p : pending) {
      if (p.id != result.id)
        continue;
      if (p.page_generation == generation) {
        for (size_t i = 0; i < control_count; ++i)
          if (controls[i].request == result.id) {
            controls[i].request = 0;
            lv_obj_remove_state(controls[i].widget, LV_STATE_DISABLED);
          }
        if (p.url && p.dialog_generation == modal_generation && modal && url_request == result.id) {
          url_request = 0;
          if (result.error == ESP_OK) {
            close_modal();
            if (page_id==8) command(Op::OTA_CHECK);
          }
          else
            lv_obj_remove_state(text_input, LV_STATE_DISABLED);
        }
        notice(result.error == ESP_OK ? "设置已保存" : "设置保存失败，请重试");
      }
      p = {};
      break;
    }
  }
}
static lv_obj_t *label(lv_obj_t *p, const char *t, int width = LV_PCT(100)) {
  auto *l = lv_label_create(p);
  lv_label_set_text(l, t);
  lv_obj_set_width(l, width);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  return l;
}
static void column(lv_obj_t *p) {
  lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(p, 12, 0);
}
static void plain(lv_obj_t *p) {
  lv_obj_remove_style_all(p);
  lv_obj_set_style_text_font(p, &font_zh, 0);
  lv_obj_set_style_text_color(p, lv_color_hex(0xe5ebf2), 0);
}
static lv_obj_t *button(lv_obj_t *p, const char *t, lv_event_cb_t cb, void *data = nullptr) {
  auto *b = lv_button_create(p);
  lv_obj_set_size(b, LV_PCT(100), 52);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x1c2632), 0);
  lv_obj_set_style_radius(b, 16, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_text_font(b, &font_zh, 0);
  auto *l = label(b, t, LV_PCT(100));
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(l);
  if (cb)
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
  return b;
}
static void show(int id);
static void go(lv_event_t *e) { show((int)(intptr_t)lv_event_get_user_data(e)); }
static void dispatch(lv_event_t *e) {
  auto op = (Op)(intptr_t)lv_event_get_user_data(e);
  if (!command(op, "", "", op == Op::BLE_FORGET ? -1 : 0))
    notice("操作队列已满，请重试");
}
static lv_obj_t *opbutton(lv_obj_t *p, const char *t, Op op) {
  return button(p, t, dispatch, (void *)(intptr_t)op);
}
static void clean() {
  ++generation;
  for (auto &ctrl : controls)
    ctrl = {};
  control_count = 0;
  if (!brightness_request)
    brightness_preview = -1;
  wifi_current = wifi_message = wifi_scan_button = nullptr;
  wifi_render_key = ~uint64_t(0);
  wifi_foreground(false);
  detail = list = nullptr;
  notice_badge = notice_list = clear_button = nullptr;
  ble_summary = ble_code = ble_pair_button = nullptr;
  for (auto &v : sys_values) v = nullptr;
  for (auto &v : sys_bars) v = nullptr;
  battery_arc = nullptr; notice_revision = ~0u;
  ota_bar = ota_status = ota_bytes = nullptr;
  ota_full_button=ota_delta_button=ota_install_button=ota_check_button=ota_options=nullptr;
  list_count = -1;
  lv_obj_clean(content);
  lv_obj_scroll_to_y(content, 0, LV_ANIM_OFF);
  lv_obj_remove_flag(headline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(content, 24, 114);
  lv_obj_set_size(content, 432, 342);
}
void app_close() {
  if (active) {
    if (active->leave)
      active->leave();
    if (active->destroy)
      active->destroy();
    active = nullptr;
  }
}
void app_open(size_t i) {
  if (i >= app_count)
    return;
  close_modal();
  app_close();
  clean();
  active = &apps[i];
  page_id = !strcmp(active->id, "settings") ? 1 : !strcmp(active->id, "status") ? 2 : !strcmp(active->id, "notifications") ? 13 : !strcmp(active->id, "logs") ? 14 : 11;
  lv_label_set_text(headline, active->name);
  active->create(content);
  if (active->enter)
    active->enter();
}
static void open_app(lv_event_t *e) { app_open((size_t)(uintptr_t)lv_event_get_user_data(e)); }
void ui_home() {
  close_modal();
  app_close();
  page_id = 0;
  clean();
  lv_label_set_text(headline, "");
  lv_obj_add_flag(headline, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(content, 24, 72);
  lv_obj_set_size(content, 432, 384);
  auto *row = lv_obj_create(content);
  plain(row);
  lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(row, 16, 0);
  lv_obj_set_style_pad_column(row, 16, 0);
  for (size_t i = 0; i < app_count; i++) {
    auto *b = button(row, apps[i].name, open_app, (void *)(uintptr_t)i);
    lv_obj_set_size(b, 208, 140);
    lv_obj_set_style_text_color(b, accent(), 0);
    auto *name = lv_obj_get_child(b, 0);
    lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -8);
    auto *icon = lv_label_create(b);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_40, 0);
    lv_label_set_text(icon, apps[i].icon);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -18);
    if (!strcmp(apps[i].id, "notifications")) {
      notice_badge = label(b, "", LV_SIZE_CONTENT);
      // Keep the empty background out of the first frame on every home entry.
      lv_obj_add_flag(notice_badge, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_text_color(notice_badge, lv_color_white(), 0);
      lv_obj_set_style_bg_color(notice_badge, lv_color_hex(0xff647c), 0);
      lv_obj_set_style_bg_opa(notice_badge, LV_OPA_COVER, 0);
      lv_obj_set_style_radius(notice_badge, 12, 0);
      lv_obj_set_style_pad_hor(notice_badge, 8, 0);
      lv_obj_align(notice_badge, LV_ALIGN_TOP_RIGHT, -4, 0);
    }
  }
  notice_badge_refresh();
}
void ui_settings(lv_obj_t *p) {
  const char *titles[] = {"Wi-Fi 网络", "蓝牙连接", "显示与息屏", "声音",
                          "TF 卡存储",  "固件升级", "系统状态",   "屏幕校准"};
  for (int i = 0; i < 8; i++)
    button(p, titles[i], go, (void *)(intptr_t)(i + 3));
  button(p, "时间同步", go, (void *)(intptr_t)12);
}
static lv_obj_t *card(lv_obj_t *p, uint32_t color, int height = LV_SIZE_CONTENT) {
  auto *v = lv_obj_create(p); plain(v);
  lv_obj_set_size(v, LV_PCT(100), height);
  lv_obj_set_style_bg_color(v, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(v, 20, 0);
  lv_obj_set_style_pad_all(v, 16, 0);
  lv_obj_remove_flag(v, LV_OBJ_FLAG_SCROLLABLE);
  return v;
}
void ui_battery(lv_obj_t *p) {
  auto *hero = card(p, 0x112d28, 168);
  battery_arc = lv_arc_create(hero);
  lv_obj_set_size(battery_arc, 130, 130);
  lv_obj_set_pos(battery_arc, 0, 2);
  lv_arc_set_range(battery_arc, 0, 100); lv_arc_set_bg_angles(battery_arc, 135, 45);
  lv_obj_remove_style(battery_arc, nullptr, LV_PART_KNOB);
  lv_obj_remove_flag(battery_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_color(battery_arc, lv_color_hex(0x25463c), LV_PART_MAIN);
  lv_obj_set_style_arc_color(battery_arc, lv_color_hex(0x71d7a4), LV_PART_INDICATOR);
  sys_values[0] = label(hero, "--", 120);
  lv_obj_set_style_text_font(sys_values[0], &lv_font_montserrat_40, 0);
  lv_obj_set_style_text_align(sys_values[0], LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(sys_values[0], 5, 40);
  auto *title = label(hero, "电源与电量", 235); lv_obj_set_pos(title, 156, 5);
  sys_values[1] = label(hero, "", 235); lv_obj_set_pos(sys_values[1], 156, 48);
  auto *row = lv_obj_create(p); plain(row); lv_obj_set_size(row, 432, 120);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW); lv_obj_set_style_pad_column(row, 12, 0);
  for (int i = 0; i < 2; ++i) {
    auto *v = card(row, i ? 0x202b48 : 0x17343f, 120); lv_obj_set_width(v, 210); column(v);
    auto *t = label(v, i ? "蓝牙" : "Wi-Fi");
    lv_obj_set_style_text_color(t, lv_color_hex(i ? 0x9eacff : 0x70d7ed), 0);
    sys_values[2+i] = label(v, "", LV_PCT(100));
    lv_label_set_long_mode(sys_values[2+i], LV_LABEL_LONG_DOT);
    lv_obj_set_height(sys_values[2+i], 54);
  }
  const char *names[] = {"内部内存", "PSRAM", "内置存储", "TF 卡"};
  const uint32_t colors[] = {0x70b5ff,0xb59aff,0xffc478,0x71d7a4};
  for (int i = 0; i < 4; ++i) {
    auto *v = card(p, 0x151e2b); column(v); label(v, names[i]);
    sys_values[4+i] = label(v, "");
    sys_bars[i] = lv_bar_create(v); lv_obj_set_size(sys_bars[i], LV_PCT(100), 10);
    lv_obj_set_style_bg_color(sys_bars[i], lv_color_hex(colors[i]), LV_PART_INDICATOR);
  }
  auto *v = card(p, 0x291f35); column(v);
  detail = label(v, "");
  opbutton(p, "重新启动", Op::REBOOT);
}
void ui_notifications(lv_obj_t *p) {
  clear_button = button(p, "清空历史", [](lv_event_t *) {
    if (notifications_clear()) {
      clearing_notices = true; lv_obj_add_state(clear_button, LV_STATE_DISABLED);
    }
  });
  notice_list = lv_obj_create(p); plain(notice_list); column(notice_list);
  lv_obj_set_size(notice_list, LV_PCT(100), LV_SIZE_CONTENT);
  notices_refresh();
}
static void notices_refresh() {
  if (!notice_list) return;
  if (clearing_notices) {
    int r = notifications_clear_result();
    if (r) {
      clearing_notices = false; lv_obj_remove_state(clear_button, LV_STATE_DISABLED);
      if (r < 0) { set_text(lv_obj_get_child(clear_button, 0), "清空失败，点击重试"); }
    }
  }
  const auto &v = notice_snapshot();
  if (notice_revision == v.revision) return;
  notice_revision = v.revision;
  lv_obj_clean(notice_list);
  if (!v.count) { auto *empty = card(notice_list, 0x151e2b); label(empty, "暂无通知"); }
  const uint32_t colors[] = {0xa9b8cb,0x70d7ed,0x9eacff,0xffc478};
  const char *names[] = {LV_SYMBOL_SETTINGS " 系统", LV_SYMBOL_WIFI " 网络", LV_SYMBOL_BLUETOOTH " 蓝牙", LV_SYMBOL_DOWNLOAD " 固件更新"};
  for (int i = (int)v.count - 1; i >= 0; --i) {
    const auto &e = v.entries[i]; auto *c = card(notice_list, 0x151e2b); column(c);
    char text[220];
    if (e.epoch) { time_t t = e.epoch; tm local{}; localtime_r(&t, &local);
      strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    } else snprintf(text, sizeof(text), "时间未同步 · 开机后 %lld 秒", e.uptime / 1000);
    auto *time_label = label(c, text); lv_obj_set_style_text_color(time_label, lv_color_hex(0x9aa9bd), 0);
    snprintf(text, sizeof(text), "%s%s", names[(unsigned)e.kind], e.repeats > 1 ? " · 重复提醒" : "");
    auto *kind = label(c, text); lv_obj_set_style_text_color(kind, lv_color_hex(colors[(unsigned)e.kind]), 0);
    label(c, e.text);
  }
  if (v.count) notifications_read(v.entries[v.count-1].id);
}
static void system_refresh(const Status &s) {
  if (!sys_values[0]) return;
  char b[240]; bool valid = s.battery.valid && s.battery.present && s.battery.percent >= 0;
  snprintf(b, sizeof(b), valid ? "%d" : "--", s.battery.percent); set_text(sys_values[0], b);
  lv_arc_set_value(battery_arc, valid ? s.battery.percent : 0);
  lv_obj_set_style_arc_color(battery_arc, lv_color_hex(ui_policy::battery_color(valid, s.battery.percent)), LV_PART_INDICATOR);
  snprintf(b, sizeof(b), "%s\n%s", !s.battery.valid ? "供电未知" : s.battery.usb ? "USB 供电" : "电池供电",
    !s.battery.valid ? "读取失败" : !s.battery.present ? "未安装电池" : s.battery.charging ? "正在充电" : s.battery.full ? "已充满" : "使用中");
  set_text(sys_values[1], b);
  snprintf(b, sizeof(b), "%s\n%s", s.wifi_connected ? "已连接" : s.wifi_busy ? "连接中" : "未连接", s.wifi_connected ? s.wifi_ssid : ""); set_text(sys_values[2], b);
  int peers = 0; for (const auto &p : s.peers) peers += p.connected;
  snprintf(b, sizeof(b), "%s\n外设 %d 台", s.phone_connected ? "手机 / 电脑在线" : config().ble ? "等待连接" : "已关闭", peers); set_text(sys_values[3], b);
  uint64_t total[] = {s.ram_total,s.psram_total,s.flash_total,s.sd_total};
  uint64_t free[] = {s.ram_free,s.psram_free,s.flash_free,s.sd_free};
  bool available[] = {s.ram_total>0,s.psram_total>0,s.flash_mounted && s.flash_capacity_valid,s.sd_mounted && s.sd_capacity_valid};
  for (int i=0;i<4;++i) {
    if (available[i] && total[i]) snprintf(b,sizeof(b),"%.2f / %.2f MB 可用",free[i]/1048576.0,total[i]/1048576.0);
    else strlcpy(b,i==2 && !s.flash_mounted ? "未初始化 / 未挂载" : i==3 && !s.sd_mounted ? "未挂载" : "容量未知",sizeof(b));
    set_text(sys_values[4+i],b); lv_bar_set_value(sys_bars[i],available[i] && total[i] ? 100-std::min(uint64_t(100),free[i]*100/total[i]) : 0,LV_ANIM_OFF);
  }
}
static void toggle(lv_obj_t *p, const char *title, const char *key, bool on) {
  auto *row = lv_obj_create(p);
  plain(row);
  lv_obj_set_size(row, LV_PCT(100), 52);
  auto *l = label(row, title, 280);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  auto *sw = lv_switch_create(row);
  lv_obj_set_size(sw, 76, 44);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  if (on)
    lv_obj_add_state(sw, LV_STATE_CHECKED);
  auto *ctrl = &controls[control_count++];
  *ctrl = {sw, nullptr, key, title, ControlType::TOGGLE};
  lv_obj_add_event_cb(
      sw,
      [](lv_event_t *e) {
        auto *ctrl = (Control *)lv_event_get_user_data(e);
        ctrl->request = submit_setting(ctrl->key, lv_obj_has_state(ctrl->widget, LV_STATE_CHECKED));
        if (ctrl->request)
          lv_obj_add_state(ctrl->widget, LV_STATE_DISABLED);
        else
          sync_controls(config(), system_prefs());
      },
      LV_EVENT_VALUE_CHANGED, ctrl);
}
static void slider(lv_obj_t *p, const char *title, const char *key, int value, int min) {
  char s[96];
  snprintf(s, sizeof(s), "%s  %d%%", title, value);
  auto *value_label = label(p, s);
  auto *sl = lv_slider_create(p);
  lv_obj_set_size(sl, LV_PCT(92), 24);
  lv_obj_set_style_margin_top(sl, 14, 0);
  lv_obj_set_style_margin_bottom(sl, 14, 0);
  lv_slider_set_range(sl, min, 100);
  lv_slider_set_value(sl, value, LV_ANIM_OFF);
  auto *ctrl = &controls[control_count++];
  *ctrl = {sl, value_label, key, title, ControlType::SLIDER};
  lv_obj_add_event_cb(
      sl,
      [](lv_event_t *e) {
        auto *ctrl = (Control *)lv_event_get_user_data(e);
        ctrl->dragging = true;
        int value = lv_slider_get_value(ctrl->widget);
        char text[80];
        snprintf(text, sizeof(text), "%s  %d%%", ctrl->title, value);
        set_text(ctrl->value_label, text);
        if (!strcmp(ctrl->key, "brightness"))
          brightness_preview = value;
      },
      LV_EVENT_VALUE_CHANGED, ctrl);
  lv_obj_add_event_cb(
      sl, [](lv_event_t *e) { ((Control *)lv_event_get_user_data(e))->dragging = true; },
      LV_EVENT_PRESSED, ctrl);
  lv_obj_add_event_cb(
      sl,
      [](lv_event_t *e) {
        auto *ctrl = (Control *)lv_event_get_user_data(e);
        ctrl->dragging = false;
        ctrl->request = submit_setting(ctrl->key, lv_slider_get_value(ctrl->widget));
        if (!strcmp(ctrl->key, "brightness")) {
          brightness_request = ctrl->request;
          if (!ctrl->request)
            brightness_preview = -1;
        }
        if (ctrl->request)
          lv_obj_add_state(ctrl->widget, LV_STATE_DISABLED);
        else
          sync_controls(config(), system_prefs());
      },
      LV_EVENT_RELEASED, ctrl);
  lv_obj_add_event_cb(
      sl,
      [](lv_event_t *e) {
        auto *ctrl = (Control *)lv_event_get_user_data(e);
        ctrl->dragging = false;
        if (!strcmp(ctrl->key, "brightness") && !ctrl->request)
          brightness_preview = -1;
      },
      LV_EVENT_PRESS_LOST, ctrl);
}
static void clamp_dropdowns() {
  for (size_t i = 0; i < control_count; ++i) {
    auto &ctrl = controls[i];
    if (ctrl.type != ControlType::SELECT || !lv_dropdown_is_open(ctrl.widget))
      continue;
    auto *popup = lv_dropdown_get_list(ctrl.widget);
    lv_obj_update_layout(popup);
    int x = lv_obj_get_x(popup), y = lv_obj_get_y(popup);
    int safe_x = std::clamp(x, 24, std::max(24, 456 - (int)lv_obj_get_width(popup)));
    int safe_y = std::clamp(y, 76, std::max(76, 456 - (int)lv_obj_get_height(popup)));
    if (x != safe_x || y != safe_y)
      lv_obj_set_pos(popup, safe_x, safe_y);
  }
}
static void select(lv_obj_t *p, const char *title, const char *options, int selected,
                   const char *key) {
  label(p, title);
  auto *d = lv_dropdown_create(p);
  lv_obj_set_size(d, LV_PCT(100), 52);
  lv_dropdown_set_options(d, options);
  lv_dropdown_set_selected(d, selected);
  lv_obj_set_style_text_font(d, &font_zh, 0);
  lv_obj_set_style_text_font(lv_dropdown_get_list(d), &font_zh, 0);
  auto *ctrl = &controls[control_count++];
  *ctrl = {d, nullptr, key, title, ControlType::SELECT};
  lv_obj_add_event_cb(
      d,
      [](lv_event_t *e) {
        auto *dropdown = (lv_obj_t *)lv_event_get_target(e);
        auto *popup = lv_dropdown_get_list(dropdown);
        lv_obj_set_parent(popup, root);
        lv_obj_set_style_max_height(popup, 200, 0);
        lv_obj_set_style_max_width(popup, 432, 0);
        lv_obj_set_style_radius(popup, 16, 0);
        // Dropdown positions its list after READY; clamp after open completes.
        lv_async_call([](void *) { clamp_dropdowns(); }, nullptr);
      },
      LV_EVENT_READY, nullptr);
  lv_obj_add_event_cb(
      d,
      [](lv_event_t *e) {
        int n = lv_dropdown_get_selected((lv_obj_t *)lv_event_get_target(e));
        auto *ctrl = (Control *)lv_event_get_user_data(e);
        const int fps[] = {15, 30, 60}, battery[] = {30, 60, 300}, usb[] = {60, 300, 600, 0};
        int value = !strcmp(ctrl->key, "log_level")       ? n
                    : !strcmp(ctrl->key, "fps")             ? fps[n]
                    : !strcmp(ctrl->key, "sleep_battery") ? battery[n]
                                                          : usb[n];
        if (!strncmp(ctrl->key, "power_", 6)) {
          const int minutes[] = {0, 1, 5, 10, 15, 30, 60};
          value = strstr(ctrl->key, "mode") ? n : minutes[n];
        }
        ctrl->request = submit_setting(ctrl->key, value);
        if (ctrl->request)
          lv_obj_add_state(ctrl->widget, LV_STATE_DISABLED);
        else
          sync_controls(config(), system_prefs());
      },
      LV_EVENT_VALUE_CHANGED, ctrl);
}
void ui_logs_destroy() {
  log_list = log_summary = log_pause_button = nullptr;
  heap_caps_free(log_view);
  log_view = nullptr;
}
void ui_logs(lv_obj_t *parent) {
  log_paused = false;
  log_revision = log_dropped = ~0u;
  log_refresh_at = 0;
  auto *view = lv_obj_create(parent); plain(view); column(view);
  lv_obj_set_size(view, LV_PCT(100), 342);
  lv_obj_set_style_pad_row(view, 8, 0);
  lv_obj_remove_flag(view, LV_OBJ_FLAG_SCROLLABLE);
  select(view, "记录级别", "关闭\nError 错误\nWarning 警告\nInfo 信息\nDebug 调试\nVerbose 详细",
         logs_level(), "log_level");
  auto *row = lv_obj_create(view); plain(row);
  lv_obj_set_size(row, LV_PCT(100), 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 12, 0);
  log_pause_button = button(row, "暂停刷新", [](lv_event_t *) {
    log_paused = !log_paused;
    set_text(lv_obj_get_child(log_pause_button, 0), log_paused ? "继续刷新" : "暂停刷新");
    if (!log_paused) { log_revision = ~0u; log_refresh_at = 0; logs_refresh(); }
  });
  lv_obj_set_size(log_pause_button, 210, 44);
  auto *clear = button(row, "清空日志", [](lv_event_t *) {
    if (!logs_clear()) { notice("日志忙，请重试"); return; }
    log_paused = false; log_revision = ~0u; log_refresh_at = 0;
    set_text(lv_obj_get_child(log_pause_button, 0), "暂停刷新");
    logs_refresh();
  });
  lv_obj_set_size(clear, 210, 44);
  log_summary = label(view, "本次开机 · 最近100条");
  lv_obj_set_height(log_summary, 26);
  log_list = card(view, 0x151e2b, 162); column(log_list);
  lv_obj_set_style_pad_all(log_list, 10, 0);
  lv_obj_add_flag(log_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(log_list, LV_DIR_VER);
  // Freeze the displayed snapshot when reading older messages. Recording continues.
  lv_obj_add_event_cb(log_list, [](lv_event_t *) {
    if (log_rendering) return;
    log_paused = true;
    set_text(lv_obj_get_child(log_pause_button, 0), "继续刷新");
  }, LV_EVENT_SCROLL_BEGIN, nullptr);
  auto *memory = heap_caps_malloc(sizeof(LogBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) { label(log_list, "日志显示内存不足"); return; }
  log_view = new (memory) LogBuffer{};
  logs_refresh();
}
static void logs_refresh() {
  if (!log_view || !log_list || log_paused || now_ms() < log_refresh_at) return;
  log_refresh_at = now_ms() + 1000;
  uint32_t dropped;
  if (!logs_snapshot(*log_view, dropped)) return;
  if (log_revision == log_view->revision && log_dropped == dropped) return;
  log_revision = log_view->revision; log_dropped = dropped;
  char summary[96];
  snprintf(summary, sizeof(summary), "%u/100 · 覆盖%lu · 丢弃%lu", log_view->count,
           (unsigned long)log_view->overwritten, (unsigned long)dropped);
  set_text(log_summary, summary);
  log_rendering = true;
  lv_obj_clean(log_list);
  if (!log_view->count) label(log_list, "暂无日志\n仅记录所选级别及更严重的日志\n重启后清空，级别保留");
  const uint32_t colors[] = {0xe5ebf2, 0xff7575, 0xffd166, 0x71d7a4, 0x70b5ff, 0xb59aff};
  for (unsigned i = log_view->count; i > 0; --i) {
    const auto &entry = log_view->at(i - 1);
    auto *text = label(log_list, entry.text);
    lv_obj_set_style_text_color(text, lv_color_hex(colors[entry.level]), 0);
  }
  lv_obj_update_layout(log_list);
  lv_obj_scroll_to_y(log_list, 0, LV_ANIM_OFF);
  log_rendering = false;
}
static void close_modal() {
  ++modal_generation;
  wifi_foreground(page_id == 3);
  wifi_security_select = nullptr;
  url_request = 0;
  if (modal) {
    lv_obj_delete(modal);
    modal = nullptr;
  }
  ssid_input = pass_input = text_input = nullptr;
}
static lv_obj_t *make_modal(const char *title) {
  close_modal();
  modal = lv_obj_create(root);
  lv_obj_set_pos(modal, 24, 24);
  lv_obj_set_size(modal, 432, 432);
  lv_obj_set_style_bg_color(modal, lv_color_hex(0x101820), 0);
  lv_obj_set_style_radius(modal, 28, 0);
  lv_obj_set_style_pad_all(modal, 12, 0);
  lv_obj_set_style_border_width(modal, 0, 0);
  column(modal);
  label(modal, title);
  return modal;
}
static lv_obj_t *input(lv_obj_t *p, const char *hint, const char *text, int max, bool secret) {
  auto *t = lv_textarea_create(p);
  lv_obj_set_size(t, LV_PCT(100), 48);
  lv_textarea_set_one_line(t, true);
  lv_textarea_set_max_length(t, max);
  lv_textarea_set_placeholder_text(t, hint);
  lv_textarea_set_password_mode(t, secret);
  lv_textarea_set_text(t, text);
  lv_obj_set_style_text_font(t, &font_zh, 0);
  return t;
}
static lv_obj_t *wifi_dialog(const char *title) {
  auto *p = make_modal(title);
  wifi_foreground(false);
  lv_obj_set_style_pad_all(p, 8, 0);
  lv_obj_set_style_pad_row(p, 4, 0);
  return p;
}
static lv_obj_t *wifi_actions(lv_obj_t *p, const char *title, lv_event_cb_t action) {
  auto *row = lv_obj_create(p);
  plain(row);
  lv_obj_set_size(row, LV_PCT(100), 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  auto *cancel = button(row, "取消", [](lv_event_t *) { close_modal(); });
  lv_obj_set_size(cancel, LV_PCT(47), 44);
  auto *ok = button(row, title, action);
  lv_obj_set_size(ok, LV_PCT(47), 44);
  return ok;
}
static void wifi_detail(uint32_t id);
static void wifi_form(const char *ssid, WifiSecurity security = WifiSecurity::PERSONAL,
                      bool hidden = false, uint32_t replacement = 0);
static void wifi_send(lv_event_t *) {
  const char *name = ssid_input ? lv_textarea_get_text(ssid_input) : wifi_edit.ssid;
  const char *password = pass_input ? lv_textarea_get_text(pass_input) : "";
  auto sec = wifi_security_select ? WifiSecurity(lv_dropdown_get_selected(wifi_security_select))
                                  : wifi_edit.security;
  if (!name[0] || strlen(name) > 32 || strlen(password) > 63 ||
      (sec != WifiSecurity::OPEN && strlen(password) < 8)) {
    notice("请检查网络名称和密码长度");
    return;
  }
  if (sec == WifiSecurity::OPEN && password[0]) {
    notice("开放网络请清空密码");
    return;
  }
  int options = int(sec) | (wifi_hidden ? 8 : 0) | (int(wifi_replace_id) << 8);
  if (command(Op::WIFI_CONNECT, name, password, options)) {
    close_modal();
    notice("正在连接网络");
  } else
    notice("操作队列已满，请重试");
}
static void wifi_form(const char *ssid, WifiSecurity security, bool hidden, uint32_t replacement) {
  wifi_edit = {};
  strlcpy(wifi_edit.ssid, ssid, sizeof wifi_edit.ssid);
  wifi_edit.security = security;
  wifi_hidden = hidden;
  wifi_replace_id = replacement;
  auto *p = wifi_dialog(hidden ? "其他网络" : "连接 Wi-Fi");
  if (hidden) {
    ssid_input = input(p, "网络名称", ssid, 32, false);
    lv_obj_set_height(ssid_input, 40);
    wifi_security_select = lv_dropdown_create(p);
    lv_obj_set_size(wifi_security_select, LV_PCT(100), 40);
    lv_dropdown_set_options(wifi_security_select, "开放网络\nWPA2 / WPA3 个人\nWPA3 个人");
    lv_dropdown_set_selected(wifi_security_select, int(security));
    lv_obj_set_style_text_font(wifi_security_select, &font_zh, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(wifi_security_select), &font_zh, 0);
    lv_obj_add_event_cb(
        wifi_security_select,
        [](lv_event_t *e) {
          auto *d = (lv_obj_t *)lv_event_get_target(e);
          auto *popup = lv_dropdown_get_list(d);
          lv_obj_set_parent(popup, root);
          lv_obj_set_style_max_height(popup, 150, 0);
          lv_obj_set_width(popup, 408);
          lv_obj_set_pos(popup, 36, 125);
        },
        LV_EVENT_READY, nullptr);
  } else {
    auto *name = label(p, ssid);
    lv_obj_set_height(name, 28);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  }
  if (security != WifiSecurity::OPEN || hidden) {
    pass_input = input(p, "密码（至少 8 个字符）", "", 63, true);
    lv_obj_set_height(pass_input, 40);
    auto *check = lv_checkbox_create(p);
    lv_checkbox_set_text(check, "显示密码");
    lv_obj_set_height(check, 28);
    lv_obj_add_event_cb(
        check,
        [](lv_event_t *e) {
          lv_textarea_set_password_mode(
              pass_input, !lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, nullptr);
    auto *kb = lv_keyboard_create(p);
    lv_obj_set_size(kb, LV_PCT(100), hidden ? 128 : 160);
    lv_keyboard_set_textarea(kb, pass_input);
    lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
    for (auto *field : {ssid_input, pass_input})
      if (field)
        lv_obj_add_event_cb(
            field,
            [](lv_event_t *e) {
              lv_keyboard_set_textarea((lv_obj_t *)lv_event_get_user_data(e),
                                       (lv_obj_t *)lv_event_get_target(e));
            },
            LV_EVENT_FOCUSED, kb);
    lv_obj_add_event_cb(kb, [](lv_event_t *) { close_modal(); }, LV_EVENT_CANCEL, nullptr);
    lv_obj_add_event_cb(kb, wifi_send, LV_EVENT_READY, nullptr);
  } else
    label(p, "这是开放网络，连接时无需密码。");
  wifi_actions(p, "连接", wifi_send);
}
static void wifi_choose_replacement(const WifiBinding &binding, bool hidden) {
  wifi_edit = binding;
  wifi_hidden = hidden;
  auto *p = wifi_dialog("选择要替换的网络");
  const auto &snap = ui_snapshot();
  auto *rows = lv_obj_create(p);
  plain(rows);
  column(rows);
  lv_obj_set_size(rows, LV_PCT(100), 304);
  for (int i = 0; i < snap.wlan.saved_count; ++i) {
    auto &v = snap.wlan.saved[i];
    button(
        rows, v.ssid,
        [](lv_event_t *e) {
          auto binding = wifi_edit;
          bool hidden = wifi_hidden;
          wifi_form(binding.ssid, binding.security, hidden,
                    (uint32_t)(uintptr_t)lv_event_get_user_data(e));
        },
        (void *)(uintptr_t)v.id);
  }
  button(p, "取消", [](lv_event_t *) { close_modal(); });
}
static void wifi_select(WifiBinding binding, bool hidden = false) {
  if (binding.security == WifiSecurity::UNSUPPORTED) {
    notice("暂不支持此网络的安全类型");
    return;
  }
  if (binding.id) {
    command(Op::WIFI_SAVED_CONNECT, "", "", binding.id);
    return;
  }
  if (ui_snapshot().wlan.saved_count >= 8)
    wifi_choose_replacement(binding, hidden);
  else
    wifi_form(binding.ssid, binding.security, hidden);
}
static void wifi_saved_list() {
  auto *p = wifi_dialog("已保存网络");
  const auto &snap = ui_snapshot();
  auto *rows = lv_obj_create(p);
  plain(rows);
  column(rows);
  lv_obj_set_size(rows, LV_PCT(100), 304);
  if (!snap.wlan.saved_count)
    label(rows, "暂无已保存网络");
  for (int i = 0; i < snap.wlan.saved_count; ++i) {
    auto &v = snap.wlan.saved[i];
    button(
        rows, v.ssid,
        [](lv_event_t *e) { wifi_detail((uint32_t)(uintptr_t)lv_event_get_user_data(e)); },
        (void *)(uintptr_t)v.id);
  }
  button(p, "完成", [](lv_event_t *) { close_modal(); });
}
static void wifi_detail(uint32_t id) {
  const auto &snap = ui_snapshot();
  WifiSaved v;
  bool found = false;
  for (int i = 0; i < snap.wlan.saved_count; ++i)
    if (snap.wlan.saved[i].id == id) {
      v = snap.wlan.saved[i];
      found = true;
    }
  if (!found) {
    notice("网络档案已更新");
    return;
  }
  auto *p = wifi_dialog("网络详情");
  label(p, v.ssid);
  char text[240];
  bool active = snap.wifi_connected && snap.wlan.current_id == id;
  if (active)
    snprintf(text, sizeof text, "已连接 · %d dBm\nIP：%s\n%s", snap.wifi_rssi, snap.ip,
             wifi_security_name(v.security));
  else
    snprintf(text, sizeof text, "%s\n%s", v.auth_failed ? "认证失败，请重新输入密码" : "已保存",
             wifi_security_name(v.security));
  label(p, text);
  auto *check = lv_checkbox_create(p);
  lv_checkbox_set_text(check, "自动连接");
  lv_obj_set_height(check, 44);
  if (v.automatic)
    lv_obj_add_state(check, LV_STATE_CHECKED);
  lv_obj_add_event_cb(
      check,
      [](lv_event_t *e) {
        bool enabled = lv_obj_has_state((lv_obj_t *)lv_event_get_target(e), LV_STATE_CHECKED);
        command(Op::WIFI_PROFILE_AUTO, enabled ? "1" : "0", "",
                (int)(uintptr_t)lv_event_get_user_data(e));
      },
      LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)id);
  if (!active)
    button(
        p, "连接",
        [](lv_event_t *e) {
          command(Op::WIFI_SAVED_CONNECT, "", "", (int)(uintptr_t)lv_event_get_user_data(e));
          close_modal();
        },
        (void *)(uintptr_t)id);
  wifi_edit = {};
  strlcpy(wifi_edit.ssid, v.ssid, 33);
  wifi_edit.security = v.security;
  wifi_hidden = v.hidden;
  if (v.security != WifiSecurity::OPEN)
    button(p, "重新输入密码", [](lv_event_t *) {
      auto b = wifi_edit;
      wifi_form(b.ssid, b.security, wifi_hidden);
    });
  button(
      p, "忘记此网络",
      [](lv_event_t *e) {
        wifi_forget_pending_id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
        auto *q = wifi_dialog("忘记此网络？");
        label(q, "设备将删除该网络的连接信息。");
        wifi_actions(q, "忘记", [](lv_event_t *) {
          command(Op::WIFI_PROFILE_FORGET, "", "", wifi_forget_pending_id);
          close_modal();
        });
      },
      (void *)(uintptr_t)id);
  button(p, "完成", [](lv_event_t *) { close_modal(); });
}
static void wifi_row(lv_obj_t *parent, const WifiBinding &binding, const char *subtitle, int rssi,
                     int slot) {
  wifi_bindings[slot] = binding;
  auto *row = lv_button_create(parent);
  lv_obj_set_size(row, LV_PCT(100), 66);
  lv_obj_set_style_bg_color(row, lv_color_hex(0x1c2632), 0);
  lv_obj_set_style_radius(row, 14, 0);
  lv_obj_set_style_shadow_width(row, 0, 0);
  lv_obj_set_style_pad_all(row, 8, 0);
  auto *name = label(row, binding.ssid, 264);
  lv_obj_set_pos(name, 2, 0);
  lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
  lv_obj_set_height(name, 26);
  auto *sub = label(row, subtitle, 270);
  lv_obj_set_pos(sub, 2, 29);
  lv_obj_set_style_text_color(sub, lv_color_hex(0xa2acb8), 0);
  lv_label_set_long_mode(sub, LV_LABEL_LONG_DOT);
  char signal[40];
  int bars = wifi_policy::bars(rssi);
  if (bars)
    snprintf(signal, sizeof signal, "%s %s\n%d dBm", "",
             (bars == 4   ? "▂▄▆█"
              : bars == 3 ? "▂▄▆"
              : bars == 2 ? "▂▄"
                          : "▂"),
             rssi);
  else
    strlcpy(signal, "--", sizeof signal);
  if (binding.security != WifiSecurity::OPEN) {
    auto *lock = lv_obj_create(row);
    plain(lock);
    lv_obj_set_pos(lock, 265, 2);
    lv_obj_set_size(lock, 16, 22);
    lv_obj_remove_flag(lock, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_event_cb(
        lock,
        [](lv_event_t *e) {
          lv_area_t a;
          lv_obj_get_coords((lv_obj_t *)lv_event_get_target(e), &a);
          auto *layer = lv_event_get_layer(e);
          lv_draw_rect_dsc_t d;
          lv_draw_rect_dsc_init(&d);
          d.border_color = accent();
          d.border_width = 2;
          d.radius = 5;
          d.bg_opa = LV_OPA_TRANSP;
          lv_area_t shackle = {a.x1 + 3, a.y1, a.x1 + 12, a.y1 + 13};
          lv_draw_rect(layer, &d, &shackle);
          d.bg_color = accent();
          d.bg_opa = LV_OPA_COVER;
          d.radius = 2;
          lv_area_t body = {a.x1, a.y1 + 9, a.x1 + 15, a.y1 + 21};
          lv_draw_rect(layer, &d, &body);
        },
        LV_EVENT_DRAW_MAIN, nullptr);
  }
  auto *strength = label(row, signal, 122);
  lv_obj_set_pos(strength, 282, 0);
  lv_obj_set_style_text_align(strength, LV_TEXT_ALIGN_RIGHT, 0);
  lv_obj_set_style_text_color(strength, accent(), 0);
  lv_obj_add_event_cb(
      row,
      [](lv_event_t *e) {
        auto b = *(WifiBinding *)lv_event_get_user_data(e);
        if (b.id && ui_snapshot().wlan.current_id == b.id)
          wifi_detail(b.id);
        else
          wifi_select(b);
      },
      LV_EVENT_CLICKED, &wifi_bindings[slot]);
}
static void wifi_refresh(const Status &s, const Config &c) {
  wifi_foreground(page_id == 3 && !modal && !asleep);
  if (!wifi_current || now_ms() < wifi_refresh_at)
    return;
  wifi_refresh_at = now_ms() + 500;
  bool fresh = s.wlan.signal_at && now_ms() - s.wlan.signal_at <= 15000;
  uint64_t key = (uint64_t(s.wlan.scan_version) << 32) ^ s.wlan.profile_version ^
                 (uint64_t(s.wlan.current_id) << 8) ^ (uint64_t(s.wlan.phase) << 24) ^
                 (uint64_t(uint8_t(fresh ? s.wifi_rssi : 0)) << 48) ^ uint64_t(c.wifi);
  set_text(wifi_message, c.wifi ? s.wifi_note : "打开 Wi-Fi 以查看附近网络");
  if (wifi_render_key == key)
    return;
  wifi_render_key = key;
  int y = lv_obj_get_scroll_y(content);
  lv_obj_clean(wifi_current);
  lv_obj_clean(list);
  if (c.wifi && s.wifi_connected) {
    WifiBinding b;
    strlcpy(b.ssid, s.wifi_ssid, 33);
    b.id = s.wlan.current_id;
    b.security = s.wlan.security;
    wifi_row(wifi_current, b, "已连接", fresh ? s.wifi_rssi : 0, 16);
  }
  if (c.wifi) {
    for (int i = 0; i < s.wlan.nearby_count; ++i) {
      auto &v = s.wlan.nearby[i];
      if (v.saved_id && v.saved_id == s.wlan.current_id && s.wifi_connected)
        continue;
      WifiBinding b;
      strlcpy(b.ssid, v.ssid, 33);
      b.security = v.security;
      b.id = 0;
      for (int j = 0; j < s.wlan.saved_count; ++j)
        if (wifi_policy::compatible(s.wlan.saved[j].security, v.security) &&
            !strcmp(v.ssid, s.wlan.saved[j].ssid))
          b.id = s.wlan.saved[j].id;
      const char *subtitle = !strcmp(s.wlan.target, v.ssid) && s.wlan.phase == WifiPhase::CONNECTING
                                 ? "正在连接…"
                             : b.id ? "已保存"
                                    : wifi_security_name(v.security);
      wifi_row(list, b, subtitle, now_ms() - s.wlan.scan_at <= 30000 ? v.rssi : 0, i);
    }
    if (!s.wlan.nearby_count)
      label(list,
            s.wlan.phase == WifiPhase::SCANNING ? "正在查找附近网络…" : "未发现网络，点击刷新重试");
  }
  lv_obj_update_layout(content);
  lv_obj_scroll_to_y(content, y, LV_ANIM_OFF);
}
static void url_form() {
  auto *p = make_modal("HTTPS 更新清单地址");
  auto c = config();
  text_input = input(p, "https://.../manifest.json",
                     c.update_url[0] ? c.update_url : UPDATE_MANIFEST_URL, 383, false);
  auto *kb = lv_keyboard_create(p);
  lv_obj_set_size(kb, LV_PCT(100), 200);
  lv_keyboard_set_textarea(kb, text_input);
  lv_obj_set_style_text_font(kb, &lv_font_montserrat_20, LV_PART_ITEMS);
  button(p, "保存地址", [](lv_event_t *) {
    if (url_request)
      return;
    auto *p = reserve_pending();
    uint32_t id = p ? update_url_async(lv_textarea_get_text(text_input)) : 0;
    if (id) {
      *p = {id, generation, modal_generation, true};
      url_request = id;
      lv_obj_add_state(text_input, LV_STATE_DISABLED);
    } else
      notice("操作队列已满或地址无效");
  });
  button(p, "取消", [](lv_event_t *) { close_modal(); });
}
static lv_obj_t *address_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, const char *address) {
  char *copy = strdup(address); if (!copy) return nullptr;
  auto *b = button(parent, text, cb, copy);
  lv_obj_add_event_cb(b, [](lv_event_t *e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, copy);
  return b;
}
static void ble_bond(lv_event_t *e) {
  char address[32]; strlcpy(address,(const char *)lv_event_get_user_data(e),sizeof(address));
  auto *p = make_modal("已配对设备"); label(p,address);
  label(p,"配对记录已保存；连接状态请查看手机 / 电脑连接卡。");
  address_button(p,"忘记此设备",[](lv_event_t *ev){
    command(Op::BLE_FORGET,(const char *)lv_event_get_user_data(ev),"",100); close_modal();
  },address);
  button(p,"完成",[](lv_event_t *){close_modal();});
}
static void ble_peer(lv_event_t *e) {
  char address[32]; strlcpy(address,(const char *)lv_event_get_user_data(e),sizeof(address));
  const auto &s = ui_snapshot();
  for (int i = 0; i < s.peer_count; i++) {
    if (strcmp(s.peers[i].address, address)) continue;
    const auto peer = s.peers[i];
    auto *p = make_modal(peer.name); label(p,address);
    char text[100]; snprintf(text,sizeof(text),"%s · %d dBm",peer.connected?"已连接":"未连接",peer.rssi); label(p,text);
    address_button(p,peer.connected?"断开连接":"连接设备",[](lv_event_t *ev){
      const char *a=(const char *)lv_event_get_user_data(ev); const auto &snap=ui_snapshot();
      for(int n=0;n<snap.peer_count;++n) if(!strcmp(a,snap.peers[n].address)) {
        command(snap.peers[n].connected?Op::BLE_DISCONNECT:Op::BLE_CONNECT,a,"",n); break;
      }
      close_modal();
    },address);
    button(p,"完成",[](lv_event_t *){close_modal();}); return;
  }
  notice("设备列表已更新，请重新选择");
}
static void show(int id) {
  close_modal();
  if (id == 0) {
    ui_home();
    return;
  }
  if (id == 1) {
    app_open(0);
    return;
  }
  if (id == 2) {
    app_open(1);
    return;
  }
  if (id == 13 || id == 14) {
    const char *app_id = id == 13 ? "notifications" : "logs";
    for (size_t i=0;i<app_count;++i) if (!strcmp(apps[i].id,app_id)) { app_open(i); return; }
  }
  if (id < 3 || id > 12 || id == 11)
    return;
  app_close();
  clean();
  page_id = id;
  auto c = config();
  auto prefs = system_prefs();
  switch (id) {
  case 3: {
    lv_label_set_text(headline, "Wi-Fi");
    if (!wifi_bindings)
      wifi_bindings = (WifiBinding *)heap_caps_calloc(17, sizeof(WifiBinding),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wifi_bindings)
      abort();
    toggle(content, "Wi-Fi", "wifi", c.wifi);
    wifi_message = label(content, "");
    lv_obj_set_style_text_color(wifi_message, lv_color_hex(0xa2acb8), 0);
    wifi_current = lv_obj_create(content);
    plain(wifi_current);
    column(wifi_current);
    lv_obj_set_size(wifi_current, LV_PCT(100), LV_SIZE_CONTENT);
    auto *heading = lv_obj_create(content);
    plain(heading);
    lv_obj_set_size(heading, LV_PCT(100), 44);
    auto *title = label(heading, "附近网络", 220);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);
    wifi_scan_button = button(heading, "刷新", [](lv_event_t *) { command(Op::WIFI_SCAN); });
    lv_obj_set_size(wifi_scan_button, 90, 44);
    lv_obj_align(wifi_scan_button, LV_ALIGN_RIGHT_MID, 0, 0);
    list = lv_obj_create(content);
    plain(list);
    column(list);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    button(content, "其他网络…", [](lv_event_t *) {
      WifiBinding b;
      b.security = WifiSecurity::PERSONAL;
      wifi_select(b, true);
    });
    button(content, "已保存网络", [](lv_event_t *) { wifi_saved_list(); });
    wifi_foreground(true);
    command(Op::WIFI_SCAN);
    break;
  }
  case 4: {
    lv_label_set_text(headline, "蓝牙连接");
    toggle(content, "蓝牙", "ble", c.ble);
    auto *hero = card(content, 0x202b48); column(hero);
    auto *name = label(hero, ble_name()); lv_obj_set_style_text_color(name, lv_color_hex(0x9eacff), 0);
    ble_summary = label(hero, "手机 / 电脑：未连接");
    ble_code = label(hero, "");
    ble_pair_button = button(content, "允许新设备配对", dispatch, (void *)(intptr_t)Op::BLE_PAIR);
    auto *row = card(content, 0x151e2b); column(row);
    opbutton(row, "扫描附近设备", Op::BLE_SCAN);
    list = lv_obj_create(content); plain(list); column(list);
    lv_obj_set_size(list, LV_PCT(100), LV_SIZE_CONTENT);
    opbutton(content, "清除全部配对记录", Op::BLE_FORGET);
    break;
  }
  case 5:
    lv_label_set_text(headline, "显示与息屏");
    slider(content, "显示亮度", "brightness", c.brightness, 5);
    select(content, "界面目标帧率", "15 FPS\n30 FPS\n60 FPS",
           c.fps == 15   ? 0
           : c.fps == 30 ? 1
                         : 2,
           "fps");
    select(content, "电池供电息屏时间", "30 秒\n1 分钟\n5 分钟",
           selected_index("sleep_battery", prefs.sleep_battery), "sleep_battery");
    select(content, "USB 供电息屏时间", "1 分钟\n5 分钟\n10 分钟\n常亮",
           selected_index("sleep_usb", prefs.sleep_usb), "sleep_usb");
    select(content, "电池休眠策略", "仅息屏\n轻睡\n深睡\n轻睡转深睡",
           power_value("power_battery_mode"), "power_battery_mode");
    select(content, "USB 休眠策略", "仅息屏\n轻睡\n深睡\n轻睡转深睡", power_value("power_usb_mode"),
           "power_usb_mode");
    select(content, "电池轻睡分钟", "关闭\n1\n5\n10\n15\n30\n60",
           selected_index("power_battery_light", power_value("power_battery_light")),
           "power_battery_light");
    select(content, "电池同步分钟", "关闭\n1\n5\n10\n15\n30\n60",
           selected_index("power_battery_sync", power_value("power_battery_sync")),
           "power_battery_sync");
    label(content, "整机休眠后按实体键唤醒。深睡无法接收蓝牙。");
    break;
  case 6:
    lv_label_set_text(headline, "声音");
    slider(content, "音量", "volume", c.volume, 0);
    toggle(content, "静音", "mute", c.mute);
    opbutton(content, "播放测试提示音", Op::AUDIO_TEST);
    break;
  case 7:
    lv_label_set_text(headline, "TF 卡存储");
    detail = label(content, "");
    opbutton(content, "挂载 / 重试", Op::SD_MOUNT);
    opbutton(content, "安全卸载", Op::SD_UNMOUNT);
    label(content, "拔卡前请安全卸载。挂载失败不会格式化存储卡。");
    break;
  case 8:
    lv_label_set_text(headline, "固件升级");
    detail = label(content, "");
    ota_status = label(content, "准备就绪");
    ota_bar = lv_bar_create(content);
    lv_obj_set_size(ota_bar, LV_PCT(100), 18);
    lv_bar_set_range(ota_bar, 0, 100);
    lv_obj_set_style_bg_color(ota_bar, accent(), LV_PART_INDICATOR);
    ota_bytes = label(content, "");
    ota_options=label(content, "正在获取更新信息");
    ota_delta_button=button(content,"增量更新",[](lv_event_t*) { ota_selected=OtaMode::DELTA; });
    ota_full_button=button(content,"全量更新",[](lv_event_t*) { ota_selected=OtaMode::FULL; });
    ota_check_button=opbutton(content,"重新检查",Op::OTA_CHECK);
    ota_install_button=button(content,"安装所选版本",[](lv_event_t*) {
      const auto &s=ui_snapshot();
      if (!s.update_available || s.ota_active || s.ota_phase==OtaPhase::CHECKING) {
        notice("请等待检查完成后再安装"); return;
      }
      if (ota_selected==OtaMode::DELTA && !s.ota_delta_available) {
        notice("增量不可用，请选择全量更新"); return;
      }
      ota_confirmed=ota_selected;
      strlcpy(ota_confirm_sha,s.ota_sha,sizeof(ota_confirm_sha));
      auto *p=make_modal("确认安装新版本？");
      char text[128];
      snprintf(text,sizeof(text),"版本 %s\n%s · 下载 %.1f KB",s.ota_version,
          ota_confirmed==OtaMode::DELTA ? "增量更新" : "全量更新",
          (ota_confirmed==OtaMode::DELTA ? s.ota_delta_size : s.ota_full_size)/1024.0);
      label(p,text);
      label(p,"升级后设备将重启。首次启动异常会自动回退。");
      button(p,"确认安装",[](lv_event_t*) {
        if(command(Op::OTA_INSTALL,"",ota_confirm_sha,(int)ota_confirmed)) close_modal();
        else notice("操作队列已满，请重试");
      });
      button(p,"取消",[](lv_event_t*) {close_modal();});
    });
    opbutton(content, "开启网页上传（十分钟）", Op::OTA_SESSION);
    button(content, "显示升级二维码", [](lv_event_t *) {
      const auto &s = ui_snapshot();
      if (!s.ota_url[0]) {
        notice("请先开启网页上传");
        return;
      }
      auto *p = make_modal("扫码打开升级页面");
      auto *q = lv_qrcode_create(p);
      lv_qrcode_set_size(q, 240);
      lv_qrcode_set_dark_color(q, lv_color_black());
      lv_qrcode_set_light_color(q, lv_color_white());
      lv_qrcode_update(q, s.ota_url, strlen(s.ota_url));
      button(p, "关闭", [](lv_event_t *) { close_modal(); });
    });
    button(content, "设置在线更新地址", [](lv_event_t *) { url_form(); });
    ota_choice_revision=~0u;
    if (!command(Op::OTA_CHECK)) notice("检查任务队列已满，请点击重新检查");
    break;
  case 9:
    lv_label_set_text(headline, "系统状态");
    ui_battery(content);
    break;
  case 12:
    lv_label_set_text(headline, "时间同步");
    detail = label(content, "");
    opbutton(content, "立即同步时间", Op::TIME_SYNC);
    label(content, "时区：北京时间 UTC+8\n联网后自动校时，离线使用设备时钟。");
    break;
  case 10: {
    lv_label_set_text(headline, "屏幕校准");
    modal = lv_obj_create(root);
    plain(modal);
    lv_obj_set_size(modal, 480, 480);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(modal, 64, 0);
    lv_obj_set_style_clip_corner(modal, true, 0);
    const char *names[] = {"左上", "右上", "左下", "右下"};
    for (int i = 0; i < 4; i++) {
      auto *b = button(modal, names[i], [](lv_event_t *e) {
        lv_obj_set_style_bg_color((lv_obj_t *)lv_event_get_target(e), lv_color_hex(0x246456), 0);
      });
      lv_obj_set_size(b, 76, 64);
      lv_obj_set_pos(b, (i % 2) ? 380 : 24, (i / 2) ? 392 : 24);
    }
    auto *l =
        label(modal, "圆角安全区域\n\n请依次点击四角。\n按钮与文字应完整可见，\n点击后变绿。", 288);
    lv_obj_set_pos(l, 96, 164);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    auto *b = button(modal, "完成", [](lv_event_t *) {
      close_modal();
      show(1);
    });
    lv_obj_set_size(b, 140, 52);
    lv_obj_set_pos(b, 170, 392);
    break;
  }
  }
}
static void wake() {
  asleep = false;
  screen_sleeping = false;
  activity = now_ms();
  if (wake_shield) {
    lv_obj_delete(wake_shield);
    wake_shield = nullptr;
  }
}
static void sleep_screen() {
  if (asleep || keep_awake)
    return;
  asleep = true;
  screen_sleeping = true;
  wake_shield = lv_obj_create(root);
  lv_obj_remove_style_all(wake_shield);
  lv_obj_set_size(wake_shield, 480, 480);
  lv_obj_set_pos(wake_shield, 0, 0);
  lv_obj_add_flag(wake_shield, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(wake_shield, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(
      wake_shield,
      [](lv_event_t *e) {
        asleep = false;
        screen_sleeping = false;
        activity = now_ms();
        lv_obj_delete_async((lv_obj_t *)lv_event_get_target(e));
        wake_shield = nullptr;
      },
      LV_EVENT_RELEASED, nullptr);
}
static bool close_dropdown() {
  for (size_t i = 0; i < control_count; ++i) {
    auto &ctrl = controls[i];
    if (ctrl.type == ControlType::SELECT && lv_dropdown_is_open(ctrl.widget)) {
      lv_dropdown_close(ctrl.widget);
      return true;
    }
  }
  return false;
}
static void input_tick(lv_timer_t *) {
  clamp_dropdowns();
  auto events = board_key_events();
  if (!events)
    return;
  activity = now_ms();
  if (events & 8) {
    if (keep_awake || ota_busy) {
      if (asleep)
        wake();
      notice("升级期间暂不能关机");
    } else if (!command(Op::POWER_OFF))
      notice("操作队列已满，请重试");
    return;
  }
  if (asleep) {
    wake();
    return; // The first physical input only wakes the screen.
  }
  if (events & 4) {
    if (!keep_awake && !ota_busy)
      sleep_screen();
  } else if (events & 2) {
    show(0);
  } else if (events & 1) {
    if (close_dropdown())
      return;
    if (modal && page_id != 10)
      close_modal();
    else
      show(ui_policy::back_page(page_id, active != nullptr));
  }
}
static void notice_badge_refresh() {
  if (!notice_badge)
    return;
  auto count = notice_snapshot().unread();
  char text[12];
  snprintf(text, sizeof(text), "%u", count);
  set_text(notice_badge, text);
  if (count)
    lv_obj_remove_flag(notice_badge, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(notice_badge, LV_OBJ_FLAG_HIDDEN);
}
static void update_notification(const Status &s) {
  if (s.notice_expires != seen_notice_expires) {
    seen_notice_expires = s.notice_expires;
    set_text(notification, s.notice);
  }
  if (now_ms() >= s.notice_expires) set_text(notification, "");
  notice_badge_refresh();
}
static void update_header(const Status &s, const Config &c) {
  char text[20];
  if (s.time_valid) {
    time_t now = time(nullptr);
    tm local{};
    localtime_r(&now, &local);
    strftime(text, sizeof(text), "%H:%M", &local);
  } else
    strlcpy(text, "--:--", sizeof(text));
  set_text(clock_label, text);
  static int previous_wifi = -1;
  int signal =
      s.wlan.signal_at && now_ms() - s.wlan.signal_at < 15000 ? wifi_policy::bars(s.wifi_rssi) : 0;
  for (int i = 0; i < 4; ++i) {
    if (s.wifi_connected) {
      lv_obj_remove_flag(wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_bg_color(wifi_bars[i], lv_color_hex(i < signal ? 0x79d5c8 : 0x354351), 0);
    } else
      lv_obj_add_flag(wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (s.wifi_connected)
    lv_obj_add_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_remove_flag(wifi_icon, LV_OBJ_FLAG_HIDDEN);
  int wifi_state = !c.wifi ? 0 : s.wifi_connected ? 3 : s.wifi_busy ? 2 : 1;
  if (wifi_state != previous_wifi) {
    lv_obj_set_style_text_color(wifi_icon,
                                lv_color_hex(wifi_state == 3   ? 0x79d5c8
                                             : wifi_state == 2 ? 0xffd166
                                                               : 0x8c98a7),
                                0);
    lv_obj_set_style_text_opa(wifi_icon, wifi_state >= 2 ? LV_OPA_COVER : LV_OPA_50, 0);
    if (wifi_state)
      lv_obj_add_flag(wifi_slash, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_remove_flag(wifi_slash, LV_OBJ_FLAG_HIDDEN);
    previous_wifi = wifi_state;
  }
  static int previous_charging = -1;
  bool charging = s.battery.valid && s.battery.charging;
  if (previous_charging != charging) {
    if (charging)
      lv_obj_remove_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);
    else
      lv_obj_add_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);
    previous_charging = charging;
  }
  bool valid = s.battery.valid && s.battery.present && s.battery.percent >= 0;
  if (valid)
    snprintf(text, sizeof(text), "%d", s.battery.percent);
  else
    strlcpy(text, "--", sizeof(text));
  set_text(battery_number, text);
  uint32_t color = ui_policy::battery_color(valid, s.battery.percent);
  static uint32_t previous_color = 0;
  if (color != previous_color) {
    lv_obj_set_style_bg_color(battery_icon, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(battery_tip, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(battery_number, lv_color_hex(valid ? 0x101820 : 0xffffff), 0);
    previous_color = color;
  }
  bool connected = false;
  for (const auto &link : s.gateways) connected |= link.encrypted && link.authenticated;
  for (const auto &p : s.peers) connected |= p.connected;
  bool pairing = s.pair_until > now_ms() || (s.phone_connected && !connected);
  lv_obj_set_style_text_color(bluetooth_icon, lv_color_hex(!c.ble ? 0x596779 : connected ? 0x70b5ff : pairing ? 0xffd166 : 0x8c98a7), 0);
  if (c.ble) lv_obj_add_flag(bluetooth_slash, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_remove_flag(bluetooth_slash, LV_OBJ_FLAG_HIDDEN);
  update_notification(s);
}
static void refresh(lv_timer_t *) {
  if (power_wake_ui.exchange(false))
    wake();
  ui_heartbeat = now_ms();
  completions();
  int requested = requested_page.exchange(-1);
  if (requested >= 0) {
    if (asleep)
      wake();
    activity = now_ms();
    show(requested);
  }
  int scroll = requested_scroll.exchange(-1);
  if (scroll >= 0) {
    if (asleep)
      wake();
    activity = now_ms();
    lv_obj_scroll_to_y(content, scroll, LV_ANIM_ON);
  }
  if (capture_requested.exchange(false) && !captured.load()) {
    lv_obj_update_layout(root);
    captured = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
  }
  int preview = wifi_preview_view.exchange(0);
  if (preview) {
    if (page_id != 3)
      show(3);
    int id = wifi_preview_id.load();
    if (preview == 1)
      wifi_detail(id);
    else if (preview == 2) {
      const auto &snap = ui_snapshot();
      if (id >= 0 && id < snap.wlan.nearby_count) {
        auto v = snap.wlan.nearby[id];
        wifi_form(v.ssid, v.security);
      }
    } else if (preview == 3)
      wifi_saved_list();
  }
  const auto &s = ui_snapshot();
  auto c = config();
  auto prefs = system_prefs();
  char b[1536];
  sync_controls(c, prefs);
  if (applied_fps != c.fps) {
    lv_timer_set_period(lv_display_get_refr_timer(lv_display_get_default()), 1000 / c.fps);
    applied_fps = c.fps;
  }
  keep_awake = ota_busy || s.pending;
  if (keep_awake) {
    if (asleep)
      wake();
    activity = now_ms();
  }
  if (s.battery.valid && s.battery.usb != last_usb) {
    last_usb = s.battery.usb;
    activity = now_ms();
  }
  uint16_t timeout = effective_sleep(s.battery);
  if (!asleep && timeout && now_ms() - activity > timeout * 1000LL && !keep_awake)
    sleep_screen();
  int bright = asleep && !power_notification_active() ? 0
               : brightness_preview >= 0              ? brightness_preview
                                                      : c.brightness;
  if (applied_brightness != bright) {
    bsp_display_brightness_set(bright);
    applied_brightness = bright;
  }
  update_header(s, c);
  if (detail) {
    switch (page_id) {
    case 0:
      snprintf(b, sizeof(b), "%s\n%s", s.wifi_connected ? s.ip : "在设置中连接网络",
               s.ota_note[0] ? s.ota_note : "系统已就绪");
      break;
    case 2:
    case 9:
      system_refresh(s);
      snprintf(b, sizeof(b), "电压：%s\nPMU 芯片温度：%s\nYA %s\n启动验证：%s", "读取中", "读取中", esp_app_get_description()->version, s.pending ? "进行中" : "已确认");
      if (s.battery.valid) snprintf(b, sizeof(b), "电压 %.3f V\nPMU 芯片温度 %.1f °C\nIP：%s\nYA %s · 启动%s", s.battery.millivolts/1000.0, s.battery.temperature, s.wifi_connected ? s.ip : "未连接", esp_app_get_description()->version, s.pending ? "验证中" : "已确认");
      break;
    case 3:
      snprintf(b, sizeof(b), "%s\n%s", s.wifi_note, s.ip);
      break;
    case 4:
      snprintf(b, sizeof(b), "%s\n手机 / 电脑：%s\n配对码：%06lu", s.ble_note,
               s.phone_connected ? "已连接" : "未连接", (unsigned long)s.pair_code);
      break;
    case 7:
      if (!s.sd_mounted)
        strlcpy(b, "未挂载 TF 卡", sizeof(b));
      else if (!s.sd_capacity_valid)
        strlcpy(b, "TF 卡已挂载\n容量读取失败", sizeof(b));
      else
        snprintf(b, sizeof(b), "TF 卡已挂载\n总容量 %.2f GB / 可用 %.2f GB", s.sd_total / 1e9,
                 s.sd_free / 1e9);
      break;
    case 8:
      snprintf(b, sizeof(b), "当前固件 %s\n%s%s", esp_app_get_description()->version,
               s.update_available ? "可用版本 " : "更新源 ",
               s.update_available ? s.ota_version : UPDATE_BASE_URL);
      break;
    case 12: {
      char current[40] = "等待校时", last_sync[40] = "尚未同步";
      tm local{};
      if (s.time_valid) {
        time_t now = time(nullptr);
        localtime_r(&now, &local);
        strftime(current, sizeof(current), "%Y-%m-%d %H:%M:%S", &local);
      }
      if (s.last_time_sync > 0) {
        time_t last = s.last_time_sync;
        localtime_r(&last, &local);
        strftime(last_sync, sizeof(last_sync), "%Y-%m-%d %H:%M:%S", &local);
      }
      snprintf(b, sizeof(b), "%s\n%s\n上次网络同步：\n%s\nRTC 保时：%s", current, s.time_note,
               last_sync, s.rtc_valid ? "有效" : "无效");
      break;
    }
    default:
      b[0] = 0;
    }
    set_text(detail, b);
  }
  if (ota_bar) {
    if (ota_choice_revision!=s.ota_revision) {
      ota_choice_revision=s.ota_revision;
      ota_selected=s.ota_delta_available ? OtaMode::DELTA : OtaMode::FULL;
    }
    if(s.ota_active) ota_selected=s.ota_mode;
    bool busy=s.ota_active || s.pending || s.ota_phase==OtaPhase::CHECKING;
    auto enabled=[](lv_obj_t *o,bool yes) {
      if (yes) lv_obj_remove_state(o,LV_STATE_DISABLED); else lv_obj_add_state(o,LV_STATE_DISABLED);
    };
    enabled(ota_delta_button,!busy && s.update_available && s.ota_delta_available);
    enabled(ota_full_button,!busy && s.update_available);
    enabled(ota_install_button,!busy && s.update_available);
    enabled(ota_check_button,!busy);
    snprintf(b,sizeof(b),"%s增量更新 · %.1f KB",ota_selected==OtaMode::DELTA ? "● " : "○ ",s.ota_delta_size/1024.0);
    set_text(lv_obj_get_child(ota_delta_button,0),b);
    snprintf(b,sizeof(b),"%s全量更新 · %.1f KB",ota_selected==OtaMode::FULL ? "● " : "○ ",s.ota_full_size/1024.0);
    set_text(lv_obj_get_child(ota_full_button,0),b);
    if(s.ota_delta_available && s.ota_full_size)
      snprintf(b,sizeof(b),"增量下载可节省 %.0f%% 流量",100.0*(s.ota_full_size-s.ota_delta_size)/s.ota_full_size);
    else strlcpy(b,s.ota_delta_note,sizeof(b));
    set_text(ota_options,b);
    int progress = std::clamp(s.ota_progress, 0, 100);
    if (lv_bar_get_value(ota_bar) != progress)
      lv_bar_set_value(ota_bar, progress, LV_ANIM_OFF);
    const char *phase = "等待操作";
    switch (s.ota_phase) {
    case OtaPhase::CHECKING:
      phase = "正在检查更新";
      break;
    case OtaPhase::AVAILABLE:
      phase = "发现新版本";
      break;
    case OtaPhase::CURRENT:
      phase = "已是最新版本";
      break;
    case OtaPhase::TRANSFERRING:
      phase = "正在传输固件";
      break;
    case OtaPhase::VERIFYING:
      phase = "正在校验固件";
      break;
    case OtaPhase::REBOOTING:
      phase = "准备重启";
      break;
    case OtaPhase::BOOT_VERIFY:
      phase = "正在验证新版本";
      break;
    case OtaPhase::COMPLETE:
      phase = "更新完成";
      break;
    case OtaPhase::FAILED:
      phase = "更新失败";
      break;
    case OtaPhase::ROLLED_BACK:
      phase = "已回退旧版本";
      break;
    default:
      break;
    }
    snprintf(b, sizeof(b), "%s\n%s", phase, s.ota_note);
    set_text(ota_status, b);
    if (s.ota_total)
      snprintf(b, sizeof(b), "%d%% · 下载 %lu / %lu 字节\n已写入 %lu 字节", progress, (unsigned long)s.ota_received,
               (unsigned long)s.ota_total, (unsigned long)s.ota_written);
    else
      strlcpy(b, s.ota_phase == OtaPhase::BOOT_VERIFY ? "启动健康检查约需 30 秒" : "", sizeof(b));
    set_text(ota_bytes, b);
  }
  if (page_id == 13) notices_refresh();
  if (page_id == 14 && !asleep) logs_refresh();
  if (page_id == 4 && ble_summary) {
    bool secure = false, ready = false;
    for (const auto &link : s.gateways) { secure |= link.authenticated && link.encrypted; ready |= link.ready; }
    set_text(ble_summary, !c.ble ? "蓝牙已关闭" : ready ? "手机 / 电脑：数据就绪" : secure ? "手机 / 电脑：安全连接" : s.phone_connected ? "手机 / 电脑：正在配对" : "手机 / 电脑：未连接");
    if (s.pair_code_valid) snprintf(b,sizeof(b),"配对码  %06lu",(unsigned long)s.pair_code);
    else if (s.pair_until > now_ms()) snprintf(b,sizeof(b),"允许新设备配对 · 剩余 %lld 秒",(s.pair_until-now_ms()+999)/1000);
    else strlcpy(b,"已配对设备可自动重连",sizeof(b));
    set_text(ble_code,b);
    set_text(lv_obj_get_child(ble_pair_button,0),s.pair_until > now_ms() ? "重新计时两分钟" : "允许新设备配对");
  }
  if (page_id == 3)
    wifi_refresh(s, c);
  if (list && page_id == 4) {
    uint32_t checksum = s.peer_count + s.bond_count * 10000 + s.ble_scan_version * 1000003u + s.ble_scanning * 13;
    for (int n = 0; n < s.bond_count; n++)
      for (const char *ch = s.bonds[n]; *ch; ch++)
        checksum = (checksum * 31 + (unsigned char)*ch) & 0x3fffffff;
    for (int i = 0; i < s.peer_count; i++)
      if (s.peers[i].connected)
        checksum += 100 + i;
    if (list_count != checksum) {
      list_count = checksum;
      lv_obj_clean(list);
      if (s.bond_count)
        label(list, "已配对设备");
      for (int n = 0; n < s.bond_count; n++) {
        address_button(list,s.bonds[n],ble_bond,s.bonds[n]);
      }
      label(list, s.ble_scanning ? "正在寻找附近设备…" : "附近的 BLE 设备");
      if (!s.peer_count) label(list,"点击扫描，查找可连接的 BLE 外设");
      for (int i = 0; i < s.peer_count; i++) {
        snprintf(b, sizeof(b), "%s", s.peers[i].name);
        char *address = strdup(s.peers[i].address);
        if (!address)
          continue;
        auto *item = button(list, b, ble_peer, address);
        lv_obj_set_style_bg_color(item, lv_color_hex(s.peers[i].connected ? 0x17394a : 0x151e2b), 0);
        lv_obj_set_height(item,78);
        auto *name=lv_obj_get_child(item,0); lv_obj_set_pos(name,0,-2);
        lv_obj_set_style_text_align(name,LV_TEXT_ALIGN_LEFT,0);
        lv_label_set_long_mode(name,LV_LABEL_LONG_DOT);
        snprintf(b,sizeof(b),"%s · %d dBm   >",s.peers[i].connected?"已连接":"未连接",s.peers[i].rssi);
        auto *sub=label(item,b);lv_obj_set_pos(sub,0,29);
        lv_obj_set_style_text_color(sub,lv_color_hex(0x9eacff),0);
        lv_obj_add_event_cb(
            item, [](lv_event_t *e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, address);
      }
    }
  }
}
void ui_start() {
  bsp_display_lock(-1);
  // Initialize BSP/SPI/Wi-Fi with the pinned internal-allocation policy first.
  // Once the initial display flush completes, keep ordinary runtime/UI objects
  // in PSRAM; explicit DMA allocations and task stacks retain their own caps.
  heap_caps_malloc_extmem_enable(0);
  auto *display = lv_display_get_default();
  auto *theme = lv_theme_default_init(display, accent(), lv_color_hex(0x315a71), true, &font_zh);
  lv_display_set_theme(display, theme);
  root = lv_obj_create(lv_screen_active());
  plain(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(root, 64, 0);
  lv_obj_set_style_clip_corner(root, true, 0);
  lv_obj_add_event_cb(root, safe_area_cover_check,
                      (lv_event_code_t)(LV_EVENT_COVER_CHECK | LV_EVENT_PREPROCESS), nullptr);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  clock_label = label(root, "--:--", 68);
  lv_obj_set_style_text_font(clock_label, &lv_font_montserrat_20, 0);
  lv_obj_set_pos(clock_label, 28, 30);
  notification = label(root, "", 190);
  lv_obj_set_pos(notification, 104, 28);
  lv_obj_set_height(notification, 30);
  lv_label_set_long_mode(notification, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_text_align(notification, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_color(notification, accent(), 0);
  status_icons = lv_obj_create(root); plain(status_icons);
  lv_obj_set_size(status_icons, LV_SIZE_CONTENT, 30);
  lv_obj_set_flex_flow(status_icons, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(status_icons, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(status_icons, 7, 0);
  lv_obj_align(status_icons, LV_ALIGN_TOP_RIGHT, -28, 28);
  auto *wifi_group = lv_obj_create(status_icons); plain(wifi_group); lv_obj_set_size(wifi_group, 28, 28);
  wifi_icon = label(wifi_group, LV_SYMBOL_WIFI, 28);
  lv_obj_set_pos(wifi_icon, 0, 0);
  lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_28, 0);
  for (int i = 0; i < 4; ++i) {
    wifi_bars[i] = lv_obj_create(wifi_group);
    plain(wifi_bars[i]);
    lv_obj_set_pos(wifi_bars[i], i * 7, 24 - (i + 1) * 5);
    lv_obj_set_size(wifi_bars[i], 5, (i + 1) * 5);
    lv_obj_set_style_bg_opa(wifi_bars[i], LV_OPA_COVER, 0);
    lv_obj_set_style_radius(wifi_bars[i], 1, 0);
    lv_obj_add_flag(wifi_bars[i], LV_OBJ_FLAG_HIDDEN);
  }
  wifi_slash = lv_line_create(wifi_group);
  static const lv_point_precise_t slash_points[] = {{0, 22}, {22, 0}};
  lv_line_set_points(wifi_slash, slash_points, 2);
  lv_obj_set_pos(wifi_slash, 2, 2);
  lv_obj_set_style_line_width(wifi_slash, 2, 0);
  lv_obj_set_style_line_color(wifi_slash, lv_color_hex(0xa2acb8), 0);
  lv_obj_add_flag(wifi_slash, LV_OBJ_FLAG_HIDDEN);
  auto *ble_group = lv_obj_create(status_icons); plain(ble_group); lv_obj_set_size(ble_group, 22, 28);
  bluetooth_icon = label(ble_group, LV_SYMBOL_BLUETOOTH, 22);
  lv_obj_set_style_text_font(bluetooth_icon, &lv_font_montserrat_28, 0);
  bluetooth_slash = lv_line_create(ble_group); lv_line_set_points(bluetooth_slash, slash_points, 2);
  lv_obj_set_style_line_width(bluetooth_slash, 2, 0);
  lv_obj_set_style_line_color(bluetooth_slash, lv_color_hex(0x8c98a7), 0);
  charging_icon = label(status_icons, LV_SYMBOL_CHARGE, 14);

  lv_obj_set_style_text_font(charging_icon, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(charging_icon, accent(), 0);
  lv_obj_add_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);
  auto *battery_group = lv_obj_create(status_icons); plain(battery_group); lv_obj_set_size(battery_group, 55, 28);
  battery_icon = lv_obj_create(battery_group);
  plain(battery_icon);
  lv_obj_set_pos(battery_icon, 0, 0);
  lv_obj_set_size(battery_icon, 50, 28);
  lv_obj_remove_flag(battery_icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_radius(battery_icon, 5, 0);
  lv_obj_set_style_bg_opa(battery_icon, LV_OPA_COVER, 0);
  battery_tip = lv_obj_create(battery_group);
  plain(battery_tip);
  lv_obj_set_pos(battery_tip, 50, 8);
  lv_obj_set_size(battery_tip, 5, 12);
  lv_obj_set_style_radius(battery_tip, 2, 0);
  lv_obj_set_style_bg_opa(battery_tip, LV_OPA_COVER, 0);
  lv_obj_remove_flag(battery_tip, LV_OBJ_FLAG_SCROLLABLE);
  battery_number = label(battery_icon, "--", LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(battery_number, &lv_font_montserrat_20, 0);
  lv_obj_center(battery_number);
  headline = label(root, "YA", 432);
  lv_obj_set_pos(headline, 24, 72);
  lv_obj_set_height(headline, 30);
  lv_obj_set_style_text_align(headline, LV_TEXT_ALIGN_CENTER, 0);
  content = lv_obj_create(root);
  plain(content);
  lv_obj_set_pos(content, 24, 114);
  lv_obj_set_size(content, 432, 342);
  lv_obj_set_style_bg_color(content, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
  column(content);
  lv_obj_set_scroll_dir(content, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);
  lv_indev_add_event_cb(
      bsp_display_get_input_dev(),
      [](lv_event_t *e) {
        if (lv_event_get_code(e) == LV_EVENT_PRESSED || lv_event_get_code(e) == LV_EVENT_PRESSING)
          activity = now_ms();
      },
      LV_EVENT_ALL, nullptr);
  lv_timer_set_period(lv_indev_get_read_timer(bsp_display_get_input_dev()), 16);
  display_metrics_init();
  activity = now_ms();
  ui_home();
  if (power_silent_boot())
    sleep_screen();
  lv_timer_create(refresh, 100, nullptr);
  lv_timer_create(input_tick, 16, nullptr);
  ui_heartbeat = now_ms();
  bsp_display_unlock();
}
} // namespace ya
