#include "data.hpp"
#include "power.hpp"
namespace ya {
namespace {
lv_obj_t *text = nullptr;
lv_timer_t *timer = nullptr;
void update(lv_timer_t *) {
  if (!text)
    return;
  auto s = data_summary();
  if (strcmp(lv_label_get_text(text), s.c_str()))
    lv_label_set_text(text, s.c_str());
}
void create(lv_obj_t *parent) {
  text = lv_label_create(parent);
  lv_obj_set_width(text, LV_PCT(100));
  lv_label_set_long_mode(text, LV_LABEL_LONG_WRAP);
  lv_label_set_text(text, "等待数据源连接");
}
void enter() {
  timer = lv_timer_create(update, 1000, nullptr);
  update(nullptr);
}
void leave() {
  if (timer) {
    lv_timer_delete(timer);
    timer = nullptr;
  }
}
void destroy() {
  leave();
  text = nullptr;
}
} // namespace
extern const App data_app = {"data", "信息", LV_SYMBOL_LIST, 0, create, enter, leave, destroy};
} // namespace ya
