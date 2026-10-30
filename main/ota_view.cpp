#include "ota_view.hpp"
#include "ui_policy.hpp"
#include <algorithm>
#include <cstring>

extern "C" const lv_font_t font_zh;
namespace ya {
namespace {
constexpr uint32_t ink = 0xe5ebf2, muted = 0x96a7b8, mint = 0x79d5c8;
void base(lv_obj_t *o) {
  lv_obj_remove_style_all(o);
  lv_obj_set_style_text_font(o, &font_zh, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(ink), 0);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
}
void visible(lv_obj_t *o, bool yes) {
  if (yes) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}
void enabled(lv_obj_t *o, bool yes) {
  if (yes) lv_obj_remove_state(o, LV_STATE_DISABLED);
  else lv_obj_add_state(o, LV_STATE_DISABLED);
}
void text(lv_obj_t *o, const char *value) {
  if (strcmp(lv_label_get_text(o), value)) lv_label_set_text(o, value);
}
lv_obj_t *label(lv_obj_t *parent, const char *value, uint32_t color = ink) {
  auto *o = lv_label_create(parent);
  lv_label_set_text(o, value);
  lv_obj_set_width(o, LV_PCT(100));
  lv_obj_set_style_text_color(o, lv_color_hex(color), 0);
  return o;
}
lv_obj_t *group(lv_obj_t *parent, bool row, int height = LV_SIZE_CONTENT) {
  auto *o = lv_obj_create(parent); base(o);
  lv_obj_set_size(o, LV_PCT(100), height);
  lv_obj_set_flex_flow(o, row ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(o, 8, 0);
  lv_obj_set_style_pad_column(o, 8, 0);
  return o;
}
lv_obj_t *button(lv_obj_t *parent, const char *value, lv_event_cb_t cb) {
  auto *o = lv_button_create(parent); base(o);
  lv_obj_set_width(o, 0);
  lv_obj_set_height(o, 46);
  lv_obj_set_flex_grow(o, 1);
  lv_obj_set_style_radius(o, 14, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(0x1a2533), 0);
  lv_obj_set_style_bg_color(o, lv_color_hex(0x293c4d), LV_STATE_PRESSED);
  lv_obj_set_style_opa(o, LV_OPA_50, LV_STATE_DISABLED);
  auto *l = label(o, value);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, nullptr);
  return o;
}
lv_obj_t *choice(lv_obj_t *parent, const char *value, lv_event_cb_t cb, lv_obj_t **size) {
  auto *o = button(parent, value, cb);
  lv_obj_set_height(o, 68);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_border_color(o, lv_color_hex(0x2b394a), 0);
  auto *title = lv_obj_get_child(o, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  *size = label(o, "--", muted);
  lv_obj_set_style_text_font(*size, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(*size, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(*size, LV_ALIGN_BOTTOM_MID, 0, -7);
  return o;
}
void selected(lv_obj_t *o, bool yes) {
  lv_obj_set_style_bg_color(o, lv_color_hex(yes ? 0x163932 : 0x17212e), 0);
  lv_obj_set_style_border_color(o, lv_color_hex(yes ? mint : 0x2b394a), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(o, 0), lv_color_hex(yes ? mint : ink), 0);
}
} // namespace

OtaView ota_view_create(lv_obj_t *parent, const char *version, const OtaViewActions &actions) {
  OtaView v{};
  v.root = group(parent, false);
  lv_obj_set_style_pad_row(v.root, 6, 0);
  auto *identity = group(v.root, true, 34);
  auto *current = label(identity, version, mint);
  lv_obj_set_width(current, 96);
  lv_label_set_long_mode(current, LV_LABEL_LONG_DOT);
  v.source = label(identity, "", muted);
  lv_obj_set_width(v.source, 0);
  lv_obj_set_flex_grow(v.source, 1);
  lv_obj_set_style_text_font(v.source, &lv_font_montserrat_20, 0);
  lv_label_set_long_mode(v.source, LV_LABEL_LONG_SCROLL_CIRCULAR);
  lv_obj_set_style_anim_duration(v.source, lv_anim_speed(32), 0);

  v.hero = group(v.root, false);
  lv_obj_set_style_bg_color(v.hero, lv_color_hex(0x122a27), 0);
  lv_obj_set_style_bg_opa(v.hero, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(v.hero, 18, 0);
  lv_obj_set_style_pad_all(v.hero, 12, 0);
  lv_obj_set_style_pad_row(v.hero, 6, 0);
  auto *heading = group(v.hero, true, 28);
  v.phase = label(heading, "准备就绪", mint);
  lv_obj_set_width(v.phase, 0);
  lv_obj_set_flex_grow(v.phase, 1);
  v.version = label(heading, "", mint);
  lv_obj_set_width(v.version, 116);
  lv_obj_set_style_text_align(v.version, LV_TEXT_ALIGN_RIGHT, 0);
  lv_label_set_long_mode(v.version, LV_LABEL_LONG_DOT);
  v.percent = label(heading, "", mint);
  lv_obj_set_width(v.percent, 68);
  lv_obj_set_style_text_font(v.percent, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_align(v.percent, LV_TEXT_ALIGN_RIGHT, 0);
  visible(v.percent, false);
  visible(v.version, false);
  v.note = label(v.hero, "正在获取更新信息", muted);
  v.transfer = group(v.hero, false);
  lv_obj_set_style_pad_row(v.transfer, 6, 0);
  v.bar = lv_bar_create(v.transfer);
  lv_obj_set_size(v.bar, LV_PCT(100), 8);
  lv_bar_set_range(v.bar, 0, 100);
  lv_obj_set_style_bg_color(v.bar, lv_color_hex(0x2c403e), LV_PART_MAIN);
  lv_obj_set_style_bg_color(v.bar, lv_color_hex(mint), LV_PART_INDICATOR);
  lv_obj_set_style_radius(v.bar, 4, LV_PART_MAIN);
  lv_obj_set_style_radius(v.bar, 4, LV_PART_INDICATOR);
  v.bytes = label(v.transfer, "", ink);
  v.written = label(v.transfer, "", muted);
  visible(v.transfer, false);

  v.choices = group(v.root, true, 68);
  v.delta = choice(v.choices, "增量更新", actions.delta, &v.delta_size);
  v.full = choice(v.choices, "全量更新", actions.full, &v.full_size);
  v.hint = label(v.root, "", muted);
  visible(v.choices, false);
  visible(v.hint, false);
  auto *commands = group(v.root, true, 46);
  v.check = button(commands, "重新检查", actions.check);
  v.install = button(commands, "安装新版本", actions.install);
  lv_obj_set_style_bg_color(v.install, lv_color_hex(mint), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(v.install, 0), lv_color_hex(0x09251e), 0);
  visible(v.install, false);
  auto *tools = group(v.root, true, 48);
  v.qr = button(tools, "二维码", actions.qr);
  v.upload = button(tools, "网页上传", actions.upload);
  v.settings = button(tools, LV_SYMBOL_SETTINGS, actions.settings);
  lv_obj_set_flex_grow(v.settings, 0);
  lv_obj_set_size(v.settings, 48, 48);
  lv_obj_set_style_text_font(lv_obj_get_child(v.settings, 0), &lv_font_montserrat_28, 0);
  return v;
}

void ota_view_update(OtaView &v, const OtaViewData &s) {
  text(v.source, s.source);
  text(v.phase, s.phase);
  text(v.note, s.note);
  visible(v.note, s.note[0] && strcmp(s.note, s.phase));
  lv_obj_set_style_bg_color(v.hero, lv_color_hex(s.failed ? 0x322226 : 0x122a27), 0);
  lv_obj_set_style_text_color(v.phase, lv_color_hex(s.failed ? 0xff9caa : mint), 0);
  bool offer = s.available && !s.checking && !s.active && !s.pending;
  visible(v.version, offer && s.version[0]);
  text(v.version, s.version);
  visible(v.choices, offer);
  visible(v.hint, offer && (s.delta_available || s.delta_note[0]));
  visible(v.install, offer);
  enabled(v.install, offer && !s.busy);
  enabled(v.check, !s.busy);
  enabled(v.delta, !s.busy && s.delta_available);
  enabled(v.full, !s.busy);
  enabled(v.qr, !s.active && !s.pending);
  enabled(v.upload, !s.active && !s.pending);
  enabled(v.settings, !s.busy);
  selected(v.delta, s.delta_selected && s.delta_available);
  selected(v.full, !s.delta_selected);
  char value[96], received[24], total[24];
  if (s.delta_available) ui_policy::format_bytes(value, sizeof value, s.delta_size);
  else std::snprintf(value, sizeof value, "--");
  text(v.delta_size, value);
  ui_policy::format_bytes(value, sizeof value, s.full_size);
  text(v.full_size, value);
  if (s.delta_available && s.full_size) {
    std::snprintf(value, sizeof value, "增量节省 %.0f%% 流量",
                  100.0 * (double(s.full_size) - s.delta_size) / s.full_size);
    text(v.hint, value);
  } else text(v.hint, s.delta_note);
  bool transfer = s.total || s.active || s.pending;
  visible(v.transfer, transfer);
  visible(v.bar, s.total || s.active);
  visible(v.percent, s.total != 0);
  int progress = std::clamp(s.progress, 0, 100);
  if (lv_bar_get_value(v.bar) != progress) lv_bar_set_value(v.bar, progress, LV_ANIM_OFF);
  std::snprintf(value, sizeof value, "%d%%", progress);
  text(v.percent, value);
  if (s.total) {
    ui_policy::format_bytes(received, sizeof received, s.received);
    ui_policy::format_bytes(total, sizeof total, s.total);
    std::snprintf(value, sizeof value, "%s / %s", received, total);
  } else std::snprintf(value, sizeof value, "%s", s.pending ? "启动健康检查约需 30 秒" : "正在准备下载");
  text(v.bytes, value);
  ui_policy::format_bytes(total, sizeof total, s.written);
  std::snprintf(value, sizeof value, "已写入 %s", total);
  text(v.written, value);
  visible(v.written, s.written != 0);
}
} // namespace ya
