#include "gallery_preview.hpp"
#include "typography.hpp"
#include <cassert>
#include <cstdio>
namespace ya {
std::atomic<bool> screen_sleeping{false};
}
namespace ya::gallery {
std::shared_ptr<Catalog> fixture = std::make_shared<Catalog>();
std::shared_ptr<View> pixels;
uint32_t seq = 0;
std::shared_ptr<const Catalog> snapshot() { return fixture; }
std::shared_ptr<const View> view_snapshot() { return pixels; }
void cancel_view() { pixels.reset(); }
uint32_t view(const std::string &aid, int page, int index, bool) {
  pixels = std::make_shared<View>();
  pixels->generation = ++seq;
  pixels->revision = 1;
  size_t count = index == -2 ? fixture->albums.size() : index < 0 ? 9 : 1;
  pixels->images.resize(count);
  pixels->errors.resize(count);
  for (size_t i = 0; i < count; ++i) {
    if (index < 0 && index != -2 && page * 9 + i >= fixture->albums[0].photos.size())
      continue;
    auto image = std::make_shared<Image>();
    image->width = index >= 0 ? 432 : 136;
    image->height = index >= 0 ? 300 : 104;
    image->pixels = {new uint8_t[image->size()], std::default_delete<uint8_t[]>()};
    auto *p = reinterpret_cast<uint16_t *>(image->pixels.get());
    for (unsigned y = 0; y < image->height; ++y)
      for (unsigned x = 0; x < image->width; ++x) {
        unsigned red = (x * 31 / image->width + (i + page * 9) * 4) % 32,
                 green = (y * 63 / image->height + i * 5) % 64,
                 blue = (x + y) * 31 / (image->width + image->height);
        p[y * image->width + x] = (red << 11) | (green << 5) | blue;
      }
    pixels->images[i] = image;
  }
  return seq;
}
} // namespace ya::gallery
static void tick() {
  for (int i = 0; i < 12; ++i) {
    lv_tick_inc(20);
    lv_timer_handler();
  }
}
static void shot(lv_obj_t *root, const char *name) {
  tick();
  lv_obj_update_layout(root);
  auto *image = lv_snapshot_take(root, LV_COLOR_FORMAT_RGB888);
  assert(image);
  FILE *f = fopen(name, "wb");
  assert(f);
  fprintf(f, "P6\n480 480\n255\n");
  for (int y = 0; y < 480; ++y)
    for (int x = 0; x < 480; ++x) {
      auto *p = image->data + y * image->header.stride + x * 3;
      uint8_t rgb[] = {p[2], p[1], p[0]};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  lv_draw_buf_destroy(image);
}
int main() {
  lv_init();
  auto *d = lv_display_create(480, 480);
  static uint8_t buffer[480 * 48 * 2];
  lv_display_set_buffers(d, buffer, nullptr, sizeof buffer, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(
      d, [](lv_display_t *d, const lv_area_t *, uint8_t *) { lv_display_flush_ready(d); });
  using namespace ya::gallery;
  fixture->albums = {{"one", "在线相册", "urls", "", {}}, {"two", "本地相册", "local", "", {}}};
  for (int i = 0; i < 10; ++i)
    fixture->albums[0].photos.push_back(
        {std::to_string(i), "https://example.com/photo.jpg", "", ""});
  auto *root = lv_obj_create(lv_screen_active());
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(root, 64, 0);
  lv_obj_set_style_clip_corner(root, true, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  auto *title = lv_label_create(root);
  lv_obj_set_style_text_font(title, ya::typography::title, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0xe5ebf2), 0);
  lv_label_set_text(title, "相册");
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
  auto *parent = lv_obj_create(root);
  lv_obj_remove_style_all(parent);
  lv_obj_set_pos(parent, 24, 72);
  lv_obj_set_size(parent, 432, 384);
  app.create(parent);
  app.enter();
  shot(root, "albums.ppm");
  auto *body = lv_obj_get_child(lv_obj_get_child(parent, 0), 0);
  lv_obj_send_event(lv_obj_get_child(body, 0), LV_EVENT_CLICKED, nullptr);
  shot(root, "grid-nine.ppm");
  lv_obj_send_event(lv_obj_get_child(body, 0), LV_EVENT_CLICKED, nullptr);
  shot(root, "large.ppm");
  assert(app.back());
  shot(root, "back-grid.ppm");
  lv_obj_send_event(lv_obj_get_child(body, 10), LV_EVENT_CLICKED, nullptr);
  shot(root, "grid-last.ppm");
  assert(app.back());
  assert(!app.back());
  for (int i = 0; i < 50; ++i) {
    app.leave();
    app.destroy();
    lv_obj_clean(parent);
    app.create(parent);
    app.enter();
    tick();
  }
  app.leave();
  app.destroy();
  lv_obj_delete(root);
  lv_deinit();
  puts("Actual gallery app: list, 9/10 images, large view, back hierarchy, 50 lifecycle cycles "
       "passed");
}
