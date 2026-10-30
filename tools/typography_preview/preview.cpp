// Offline review of the actual generated fonts and production OTA/boot views.
// Outputs PPM files in the working directory; never connects to a device.
#include "typography.hpp"
#include "boot_view.hpp"
#include "ota_view.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace type = ya::typography;
static void flush(lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); }
static void shot(lv_obj_t *root, const char *name) {
  lv_obj_update_layout(root);
  auto *image = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB888); assert(image);
  char path[128]; snprintf(path, sizeof path, "%s.ppm", name);
  auto *file = fopen(path, "wb"); assert(file);
  fprintf(file, "P6\n480 480\n255\n");
  for (int y = 0; y < 480; ++y) for (int x = 0; x < 480; ++x) {
    auto *p = image->data + y * image->header.stride + x * 3;
    uint8_t rgb[] = {p[2], p[1], p[0]}; fwrite(rgb, 1, 3, file);
  }
  fclose(file); lv_draw_buf_destroy(image);
}
static lv_obj_t *label(lv_obj_t *p, const char *s, const lv_font_t *font, int x, int y, int w) {
  auto *o = lv_label_create(p); lv_label_set_text(o, s);
  lv_obj_set_style_text_font(o, font, 0); lv_obj_set_pos(o, x, y); lv_obj_set_width(o, w);
  return o;
}
static lv_obj_t *root() {
  auto *o = lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(o);
  lv_obj_set_size(o, 480, 480); lv_obj_set_style_bg_color(o, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0); lv_obj_set_style_radius(o, 64, 0);
  lv_obj_set_style_clip_corner(o, true, 0); lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_text_font(o, type::body, 0);
  lv_obj_set_style_text_color(o, lv_color_hex(0xe5ebf2), 0); return o;
}
static void glyph_checks() {
  assert(type::px(12) == 24);
  for (auto *font : {type::caption, type::body, type::title}) {
    for (uint32_t cp : {0x4e2du, 0x7535u, 0x7f51u, 0x2026u, uint32_t('0'), uint32_t('A')}) {
      lv_font_glyph_dsc_t glyph{};
      assert(lv_font_get_glyph_dsc(font, &glyph, cp, 0));
      assert(glyph.resolved_font == font && !glyph.is_placeholder);
    }
    lv_font_glyph_dsc_t icon{};
    assert(lv_font_get_glyph_dsc(font, &icon, 0xf013, 0)); // Settings symbol.
    assert(icon.resolved_font == font->fallback && !icon.is_placeholder);
  }
  puts("CJK, ASCII, ellipsis and icon fallback glyphs present in all roles");
  assert(lv_text_get_width("23:59", 5, type::clock, 0) <= 76);
  assert(lv_text_get_width("100", 3, type::number_small, 0) <= 50);
  assert(lv_text_get_width("100%", 4, type::number_large, 0) <= 120);
  assert(lv_text_get_width("刷新", strlen("刷新"), type::title, 0) <= 90 - 32);
  assert(lv_text_get_width("右下", strlen("右下"), type::title, 0) <= 76 - 16);
  assert(lv_font_get_line_height(type::title) <= 44 - 8);
  assert(lv_font_get_line_height(type::body) <= 40 - 12);
  assert(lv_font_get_line_height(type::caption) * 2 <= 72 - 16);
  puts("Clock, 100% battery, compact buttons, 40px input and two-line RSSI fit native bounds");
}
static void specimen() {
  auto *o = root();
  label(o, "10:42", type::clock, 28, 30, 76);
  label(o, "字体预览", type::title, 176, 28, 128);
  label(o, "88%", type::number_small, 390, 32, 60);
  label(o, "Noto Sans SC + Inter", type::caption, 24, 84, 432);
  label(o, "网络与连接", type::title, 24, 128, 432);
  label(o, "蓝牙已连接 · Wi-Fi 网络", type::body, 24, 177, 432);
  label(o, "电池剩余 88%，北京时间", type::body, 24, 222, 432);
  label(o, "估算随使用与充电情况变化", type::caption, 24, 269, 432);
  label(o, "24px Regular / 28px Medium", type::caption, 24, 306, 432);
  label(o, "88%  22.5", type::number_large, 24, 353, 432);
  label(o, "0123456789  AaBb  : / . %", type::caption, 24, 420, 432);
  lv_obj_update_layout(o);
  for (unsigned i = 0; i < lv_obj_get_child_count(o); ++i) {
    lv_area_t a; lv_obj_get_coords(lv_obj_get_child(o, i), &a);
    assert(a.x1 >= 24 && a.y1 >= 24 && a.x2 <= 455 && a.y2 <= 455);
  }
  shot(o, "specimen"); lv_obj_delete(o);
}
static void ota() {
  auto *o = root();
  label(o, "10:42", type::clock, 28, 30, 76);
  label(o, "固件升级", type::title, 176, 28, 128);
  auto *content = lv_obj_create(o); lv_obj_remove_style_all(content);
  lv_obj_set_pos(content, 24, 72); lv_obj_set_size(content, 432, 384);
  lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN); lv_obj_set_scroll_dir(content, LV_DIR_VER);
  auto view = ya::ota_view_create(content, "0.7.0", {});
  ya::OtaViewData s{};
  s.source = "https://ya.cloverstd.com/api/v1/ota/manifest.json";
  s.phase = s.note = "正在检查更新"; s.version = s.delta_note = "";
  s.checking = s.busy = true;
  auto save = [&](const char *name) {
    ya::ota_view_update(view, s); lv_obj_update_layout(o); shot(o, name);
    printf("%s: view height=%d, scroll=%d\n", name, int(lv_obj_get_height(view.root)), int(lv_obj_get_scroll_bottom(content)));
  };
  save("ota-checking");
  s.checking = s.busy = false; s.phase = "已是最新版本"; s.note = ""; save("ota-current");
  s.available = s.delta_available = s.delta_selected = true;
  s.phase = "发现新版本"; s.note = "发现可用版本，请确认安装"; s.version = "0.7.1";
  s.full_size = 2600000; s.delta_size = 114845; save("ota-available");
  assert(!lv_obj_has_flag(view.install, LV_OBJ_FLAG_HIDDEN));
  lv_area_t delta, full, settings; lv_obj_get_coords(view.delta, &delta);
  lv_obj_get_coords(view.full, &full); lv_obj_get_coords(view.settings, &settings);
  assert(delta.y1 == full.y1 && delta.x2 < full.x1 && settings.x2 <= 455);
  assert(lv_obj_get_scroll_bottom(content) <= 0);
  s.delta_available = s.delta_selected = false; s.delta_note = "没有匹配当前固件的增量包";
  save("ota-full-only"); assert(lv_obj_has_state(view.delta, LV_STATE_DISABLED));
  s.busy = s.active = true; s.phase = "正在传输固件"; s.note = "正在下载并重建增量固件";
  s.received = 73501; s.total = 114845; s.written = 1428685; s.progress = 64;
  save("ota-transfer"); assert(lv_obj_has_flag(view.install, LV_OBJ_FLAG_HIDDEN));
  s.active = s.busy = false; s.failed = true; s.phase = "更新失败";
  s.note = "固件传输中断或超时，保留原固件"; save("ota-failure");
  s.failed = false; s.pending = s.busy = true; s.phase = "正在验证新版本";
  s.note = "新固件正在进行三十秒启动验证"; s.received = s.total = s.written = s.progress = 0;
  save("ota-boot-verify");
  lv_obj_delete(o);
}
int main() {
  lv_init(); auto *display = lv_display_create(480, 480);
  static uint8_t buffer[480 * 12 * 2];
  lv_display_set_buffers(display, buffer, nullptr, sizeof buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(display, flush);
  glyph_checks(); specimen(); ota();
  auto *screen = lv_screen_active(); auto count = lv_obj_get_child_count(screen);
  auto animations = lv_anim_count_running();
  auto *boot = ya::boot_view_create(screen, type::body); shot(boot, "boot");
  for (unsigned i = 0; i < lv_obj_get_child_count(boot); ++i) {
    lv_area_t a; lv_obj_get_coords(lv_obj_get_child(boot, i), &a);
    assert(a.x1 >= 24 && a.y1 >= 24 && a.x2 <= 455 && a.y2 <= 455);
  }
  lv_obj_delete(boot);
  assert(lv_obj_get_child_count(screen) == count && lv_anim_count_running() == animations);
  assert(lv_mem_test() == LV_RESULT_OK);
  lv_display_delete(display); lv_deinit(); puts("Offline typography checks passed");
}
