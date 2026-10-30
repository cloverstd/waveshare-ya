#include "companion.hpp"
namespace ya {
namespace {
lv_obj_t *text = nullptr, *image = nullptr;
lv_timer_t *timer = nullptr;
std::shared_ptr<const RichMessage> shown;
lv_image_dsc_t descriptor{};
void update(lv_timer_t *) {
  auto message = companion_snapshot();
  if (!message || message == shown) return;
  // Keep the previous pixels alive until LVGL has stopped referencing them.
  lv_image_cache_drop(&descriptor);
  descriptor = {};
  descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
  descriptor.header.w = 432; descriptor.header.h = message->height;
  descriptor.header.stride = 864;
  descriptor.data_size = 864 * message->height; descriptor.data = message->pixels.get();
  lv_image_set_src(image, &descriptor);
  shown = message;
  lv_obj_add_flag(text, LV_OBJ_FLAG_HIDDEN); lv_obj_remove_flag(image, LV_OBJ_FLAG_HIDDEN);
}
void create(lv_obj_t *parent) {
  text = lv_label_create(parent); lv_obj_set_width(text, LV_PCT(100));
  lv_label_set_text(text, "在蓝牙数据站发送文字\n支持浏览器预览中的中文和 Emoji");
  image = lv_image_create(parent); lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
}
void enter() { timer = lv_timer_create(update, 200, nullptr); update(nullptr); }
void leave() { if (timer) { lv_timer_delete(timer); timer = nullptr; } }
void destroy() { leave(); lv_image_cache_drop(&descriptor); shown.reset(); text = image = nullptr; }
}
extern const App data_app = {"data", "信息", LV_SYMBOL_LIST, 0, create, enter, leave, destroy};
}
