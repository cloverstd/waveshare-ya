#include "status_view.hpp"
#include "typography.hpp"
#include "ui_policy.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace ya {
namespace {
constexpr uint32_t ink = 0xe5ebf2, muted = 0x96a7b8, mint = 0x71d7a4;
void plain(lv_obj_t *o) {
  lv_obj_remove_style_all(o);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(o, typography::caption, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(ink), 0);
}
lv_obj_t *label(lv_obj_t *p, const char *text, int y, const lv_font_t *font = typography::caption) {
  auto *o = lv_label_create(p);
  lv_label_set_text(o, text);
  lv_obj_set_size(o, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_text_font(o, font, 0);
  lv_obj_set_style_text_align(o, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_y(o, y);
  return o;
}
void text(lv_obj_t *o, const char *value) {
  if (strcmp(lv_label_get_text(o), value)) lv_label_set_text(o, value);
}
lv_obj_t *row(lv_obj_t *p, int height) {
  auto *o = lv_obj_create(p); plain(o);
  lv_obj_set_size(o, LV_PCT(100), height);
  lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(o, 12, 0);
  return o;
}
lv_obj_t *card(lv_obj_t *p, int height, uint32_t color) {
  auto *o = lv_obj_create(p); plain(o);
  lv_obj_set_size(o, 0, height); lv_obj_set_flex_grow(o, 1);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(o, 20, 0);
  lv_obj_set_style_pad_all(o, 12, 0);
  return o;
}
lv_obj_t *ring(lv_obj_t *p, uint32_t color) {
  auto *o = lv_arc_create(p); plain(o);
  lv_obj_set_size(o, 140, 140); lv_obj_align(o, LV_ALIGN_TOP_MID, 0, 0);
  lv_arc_set_range(o, 0, 100); lv_arc_set_bg_angles(o, 0, 360); lv_arc_set_rotation(o, 270);
  lv_obj_set_style_arc_width(o, 9, LV_PART_MAIN);
  lv_obj_set_style_arc_width(o, 9, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(o, lv_color_hex(0x2c3c4c), LV_PART_MAIN);
  lv_obj_set_style_arc_color(o, lv_color_hex(color), LV_PART_INDICATOR);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}
lv_obj_t *icon(lv_obj_t *p, const char *symbol, int y, uint32_t color) {
  auto *o = label(p, symbol, y, &lv_font_montserrat_28);
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  return o;
}
// Small native vector icons: internal RAM, external RAM, flash, and TF card.
// They share the firmware's LVGL renderer and need no raster assets or decoder.
void resource_icon(lv_obj_t *p, int kind, uint32_t color) {
  auto *body = lv_obj_create(p); plain(body);
  lv_obj_set_size(body, kind < 2 ? 28 : 26, 28);
  lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 29);
  lv_obj_set_style_radius(body, kind < 2 ? 3 : 5, 0);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, lv_color_hex(color), 0);
  if (kind < 2) {
    for (int side : {-1, 1}) for (int n = 0; n < 3; ++n) {
      auto *pin = lv_obj_create(p); plain(pin); lv_obj_set_size(pin, 5, 3);
      lv_obj_align(pin, LV_ALIGN_TOP_MID, side * 18, 34 + n * 8);
      lv_obj_set_style_bg_color(pin, lv_color_hex(color), 0);
      lv_obj_set_style_bg_opa(pin, LV_OPA_COVER, 0);
    }
    auto *center = lv_obj_create(body); plain(center);
    lv_obj_set_size(center, kind == 0 ? 10 : 16, 10); lv_obj_center(center);
    lv_obj_set_style_border_width(center, kind == 0 ? 0 : 2, 0);
    lv_obj_set_style_border_color(center, lv_color_hex(color), 0);
    lv_obj_set_style_bg_color(center, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(center, kind == 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  } else if (kind == 2) {
    auto *light = lv_obj_create(body); plain(light); lv_obj_set_size(light, 14, 3);
    lv_obj_align(light, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_bg_color(light, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(light, LV_OPA_COVER, 0);
  } else {
    for (int n = 0; n < 3; ++n) {
      auto *contact = lv_obj_create(body); plain(contact); lv_obj_set_size(contact, 3, 9);
      lv_obj_set_pos(contact, 4 + n * 6, 2);
      lv_obj_set_style_bg_color(contact, lv_color_hex(color), 0);
      lv_obj_set_style_bg_opa(contact, LV_OPA_COVER, 0);
    }
  }
}
void duration(char *out, size_t n, int64_t minutes) {
  if (minutes >= 60) snprintf(out, n, "%lldh %02lldm", minutes / 60, minutes % 60);
  else snprintf(out, n, "%lldm", minutes);
}
} // namespace
StatusView status_view_create(lv_obj_t *parent) {
  StatusView v{};
  auto *power = row(parent, 240);
  auto *battery = card(power, 240, 0x112d28);
  v.battery = ring(battery, mint);
  v.source = icon(battery, LV_SYMBOL_BATTERY_FULL, 29, mint);
  v.percent = label(battery, "--", 64, typography::number_large);
  v.charge = icon(battery, "", 158, mint);
  auto *usage = card(power, 240, 0x182d39);
  v.usage = ring(usage, 0x70b5ff);
  v.direction = icon(usage, LV_SYMBOL_DOWN, 29, 0x70b5ff);
  v.change = label(usage, "--", 69, typography::brand);
  v.eta = label(usage, "正在估算", 144);
  v.session = label(usage, "", 168, typography::number_small);
  v.rate = label(usage, "", 192, typography::number_small);
  const uint32_t colors[] = {0x70b5ff, 0xb59aff, 0xffc478, mint};
  for (int pair = 0; pair < 2; ++pair) {
    auto *r = row(parent, 190);
    for (int col = 0; col < 2; ++col) {
      int i = pair * 2 + col;
      auto *c = card(r, 190, 0x151e2b);
      v.resources[i] = ring(c, colors[i]); resource_icon(c, i, colors[i]);
      v.values[i] = label(c, "--", 69, typography::brand);
      v.notes[i] = label(c, "", 142);
      lv_obj_set_style_text_color(v.notes[i], lv_color_hex(muted), 0);
    }
  }
  auto *links = row(parent, 108);
  for (int i = 0; i < 2; ++i) {
    auto *c = card(links, 108, i ? 0x202b48 : 0x17343f);
    icon(c, i ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI, 0, i ? 0x9eacff : 0x70d7ed);
    v.links[i] = label(c, "", 38);
    lv_label_set_long_mode(v.links[i], LV_LABEL_LONG_DOT);
    lv_obj_set_height(v.links[i], 48);
  }
  return v;
}
void status_view_update(StatusView &v, const StatusViewData &s) {
  char b[96], time[32];
  bool valid = s.valid && s.present && s.percent >= 0 && s.percent <= 100;
  auto color = ui_policy::battery_color(valid, s.percent);
  lv_arc_set_value(v.battery, valid ? s.percent : 0);
  lv_obj_set_style_arc_color(v.battery, lv_color_hex(color), LV_PART_INDICATOR);
  snprintf(b, sizeof b, valid ? "%d%%" : "--", s.percent); text(v.percent, b);
  text(v.source, !s.valid ? LV_SYMBOL_WARNING : s.usb ? LV_SYMBOL_USB : LV_SYMBOL_BATTERY_FULL);
  text(v.charge, !s.valid ? LV_SYMBOL_WARNING : !s.present ? LV_SYMBOL_CLOSE :
       s.charging ? LV_SYMBOL_CHARGE : s.stats.mode == battery_stats::Mode::FULL ? LV_SYMBOL_OK : "");
  const auto &st = s.stats;
  bool tracking = valid && (st.mode == battery_stats::Mode::CHARGING || st.mode == battery_stats::Mode::DISCHARGING);
  bool charging = st.mode == battery_stats::Mode::CHARGING;
  text(v.direction, tracking ? charging ? LV_SYMBOL_UP : LV_SYMBOL_DOWN : LV_SYMBOL_PAUSE);
  lv_arc_set_value(v.usage, tracking ? std::clamp(st.change_percent, 0, 100) : 0);
  snprintf(b, sizeof b, tracking ? "%s%d%%" : "--", charging ? "+" : "-", st.change_percent); text(v.change, b);
  using battery_stats::Phase;
  if (valid && st.phase == Phase::READY && st.remaining_minutes >= 0) {
    duration(time, sizeof time, st.remaining_minutes);
    snprintf(b, sizeof b, LV_SYMBOL_EYE_OPEN " ~%s", time);
  } else snprintf(b, sizeof b, "%s", !s.valid ? "读取失败" : !s.present ? "未安装电池" :
       !valid ? "电量未知" : st.phase == Phase::FULL ? "已充满" :
       st.phase == Phase::CONFIRMING_FULL ? "等待充满确认" :
       st.phase == Phase::NO_PROGRESS ? "电量暂无增长" : tracking ? "正在估算" : "--");
  text(v.eta, b);
  duration(time, sizeof time, st.session_seconds / 60);
  snprintf(b, sizeof b, tracking ? LV_SYMBOL_LOOP " %s" : "", time); text(v.session, b);
  if (tracking && st.recent_rate >= 0) snprintf(b, sizeof b, LV_SYMBOL_REFRESH " %.1f%%/h", st.recent_rate);
  else snprintf(b, sizeof b, tracking ? LV_SYMBOL_REFRESH " --%%/h" : "");
  text(v.rate, b);
  for (int i = 0; i < 4; ++i) {
    bool available = s.available[i] && s.total[i];
    int used = available ? int(100 - std::min(uint64_t(100), s.free[i] * 100 / s.total[i])) : 0;
    lv_arc_set_value(v.resources[i], used);
    snprintf(b, sizeof b, available ? "%d%%" : "--", used); text(v.values[i], b);
    text(v.notes[i], available ? "" : i < 2 ? "容量未知" : "未挂载");
  }
  text(v.links[0], s.wifi); text(v.links[1], s.bluetooth);
}
} // namespace ya
