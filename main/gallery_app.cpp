#ifdef YA_GALLERY_PREVIEW
#include "gallery_preview.hpp"
#else
#include "gallery.hpp"
#include "typography.hpp"
#include "power.hpp"
#include "bsp/display.h"
#endif
#include "typography.hpp"
namespace ya::gallery {
namespace {
lv_obj_t *root = nullptr, *body = nullptr, *pictures[9]{}, *labels[9]{};
lv_timer_t *timer = nullptr;
lv_obj_t *position = nullptr;
int slide_direction = 0;
lv_image_dsc_t descriptors[9]{};
std::shared_ptr<const Catalog> shown;
std::shared_ptr<const View> frame;
std::string selected;
int page = 0, index = -1;
uint32_t gen = 0, view_revision = 0;
bool sleeping = false;
void draw();
void clear_images() {
  for (auto &d : descriptors) {
    lv_image_cache_drop(&d);
    d = {};
  }
  frame.reset();
  for (auto &p : pictures)
    p = nullptr;
  for (auto &l : labels)
    l = nullptr;
}
lv_obj_t *text(lv_obj_t *parent, const char *s, int width = 432) {
  auto *l = lv_label_create(parent);
  lv_obj_set_width(l, width);
  lv_obj_set_style_text_font(l, typography::caption, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(0xe5ebf2), 0);
  lv_label_set_text(l, s);
  return l;
}
void plain(lv_obj_t *obj) {
  lv_obj_remove_style_all(obj);
  lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
  lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}
const Album *current() {
  if (shown)
    for (const auto &a : shown->albums)
      if (a.id == selected)
        return &a;
  return nullptr;
}
void fetch_view(bool force = false) {
  view_revision = 0;
  gen = view(selected, page, selected.empty() ? -2 : index, force);
}
void choose(lv_event_t *e) {
  size_t i = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  if (shown && i < shown->albums.size()) {
    selected = shown->albums[i].id;
    page = 0;
    index = -1;
    draw();
  }
}
void enlarge(lv_event_t *e) {
  auto *a = current();
  int i = page * 9 + int(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  if (a && i < int(a->photos.size())) {
    index = i;
    draw();
  }
}
void turn(lv_event_t *e) {
  auto *a = current();
  if (!a || a->photos.empty())
    return;
  int step = int(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
  page = wrap(page + step, pages(a->photos.size()));
  draw();
}
void gesture(lv_event_t *) {
  auto *input = lv_indev_active();
  if (!input)
    return;
  auto dir = lv_indev_get_gesture_dir(input);
  if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT)
    return;
  auto *a = current();
  if (!a || a->photos.empty())
    return;
  int step = dir == LV_DIR_LEFT ? 1 : -1;
  lv_indev_wait_release(input);
  if (index >= 0) {
    index = wrap(index + step, a->photos.size());
    slide_direction = step;
    fetch_view();
    if (labels[0]) {
      lv_label_set_text(labels[0], "加载中");
      lv_obj_remove_flag(labels[0], LV_OBJ_FLAG_HIDDEN);
    }
  } else {
    page = wrap(page + step, pages(a->photos.size()));
    draw();
  }
}
void retry(lv_event_t *) { fetch_view(true); }
void draw() {
  if (!body)
    return;
  lv_obj_remove_event_cb(body, retry);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clean(body);
  clear_images();
  view_revision = 0;
  position = nullptr;
  slide_direction = 0;
  if (!shown) {
    text(body, "正在读取相册…");
    return;
  }
  if (!shown->error.empty()) {
    auto *l = text(body, shown->error.c_str());
    lv_obj_center(l);
    return;
  }
  const auto *a = current();
  if (!selected.empty() && !a) {
    selected.clear();
    index = -1;
    page = 0;
  }
  if (selected.empty()) {
    if (shown->albums.empty()) {
      auto *l = text(body, "暂无相册\n\n在蓝牙数据站创建相册\n上传照片、配置 URL 或从 TF 卡导入");
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
      return;
    }
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    for (size_t i = 0; i < shown->albums.size(); ++i) {
      const auto &v = shown->albums[i];
      auto *b = lv_button_create(body);
      lv_obj_set_pos(b, 0, i * 88);
      lv_obj_set_size(b, 432, 80);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x18232e), 0);
      lv_obj_set_style_radius(b, 14, 0);
      lv_obj_set_style_pad_all(b, 0, 0);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_add_event_cb(b, choose, LV_EVENT_CLICKED, reinterpret_cast<void *>(i));
      auto *clip = lv_obj_create(b);
      plain(clip);
      lv_obj_set_pos(clip, 8, 8);
      lv_obj_set_size(clip, 64, 64);
      pictures[i] = lv_image_create(clip);
      lv_obj_center(pictures[i]);
      auto *name = text(b, v.name.c_str(), 330);
      lv_obj_set_pos(name, 88, 10);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      char count[80];
      snprintf(count, sizeof count, "%s · %u 张", v.source == "local" ? "本地" : "在线",
               unsigned(v.photos.size()));
      auto *sub = text(b, count, 330);
      lv_obj_set_pos(sub, 88, 43);
      lv_obj_set_style_text_color(sub, lv_color_hex(0x9aabba), 0);
    }
    gen = view("", 0, -2);
    return;
  }
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  if (a->photos.empty()) {
    auto *l = text(body, a->source == "manifest" ? "相册为空\n点击刷新远程清单"
                                                 : "相册为空\n在蓝牙数据站添加照片");
    lv_obj_center(l);
    if (a->source == "manifest") {
      lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(body, retry, LV_EVENT_CLICKED, nullptr);
    }
    fetch_view();
    return;
  }
  page = std::min(page, int(pages(a->photos.size())) - 1);
  if (index >= int(a->photos.size()))
    index = a->photos.size() - 1;
  if (index < 0) {
    for (int i = 0; i < 9; ++i) {
      int photo = page * 9 + i;
      auto *b = lv_obj_create(body);
      plain(b);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x17222c), 0);
      lv_obj_set_style_radius(b, 8, 0);
      lv_obj_set_style_clip_corner(b, true, 0);
      lv_obj_set_pos(b, (i % 3) * 148, (i / 3) * 112);
      lv_obj_set_size(b, 136, 104);
      if (photo >= int(a->photos.size())) {
        lv_obj_set_style_bg_opa(b, LV_OPA_20, 0);
        continue;
      }
      lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(b, enlarge, LV_EVENT_CLICKED, reinterpret_cast<void *>(uintptr_t(i)));
      lv_obj_add_event_cb(b, gesture, LV_EVENT_GESTURE, nullptr);
      pictures[i] = lv_image_create(b);
      lv_obj_center(pictures[i]);
      labels[i] = text(b, "加载中", 128);
      lv_obj_set_style_text_align(labels[i], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(labels[i]);
    }
    for (int dir : {-1, 1}) {
      auto *b = lv_button_create(body);
      lv_obj_set_pos(b, dir < 0 ? 0 : 344, 340);
      lv_obj_set_size(b, 88, 44);
      lv_obj_set_style_shadow_width(b, 0, 0);
      lv_obj_set_style_bg_color(b, lv_color_hex(0x1c3037), 0);
      auto *l = text(b, dir < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, 44);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_center(l);
      lv_obj_add_event_cb(b, turn, LV_EVENT_CLICKED, reinterpret_cast<void *>(intptr_t(dir)));
    }
    char s[48];
    snprintf(s, sizeof s, "%d / %u 页", page + 1, unsigned(pages(a->photos.size())));
    auto *l = text(body, s, 240);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 96, 350);
  } else {
    auto *clip = lv_obj_create(body);
    plain(clip);
    lv_obj_set_size(clip, 432, 340);
    lv_obj_add_flag(clip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clip, gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_add_event_cb(clip, retry, LV_EVENT_CLICKED, nullptr);
    pictures[0] = lv_image_create(clip);
    lv_obj_center(pictures[0]);
    labels[0] = text(clip, "加载中");
    lv_obj_set_style_text_align(labels[0], LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(labels[0]);
    char s[80];
    snprintf(s, sizeof s, "%d / %u · 左右滑动", index + 1, unsigned(a->photos.size()));
    auto *l = text(body, s);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(l, 0, 350);
    position = l;
  }
  fetch_view();
}
void update(lv_timer_t *) {
  bool now = screen_sleeping;
  if (now) {
    sleeping = true;
    return;
  }
  if (sleeping) {
    sleeping = false;
    draw();
  }
  auto c = snapshot();
  if (c && c != shown) {
    shown = c;
    draw();
  }
  if (!gen)
    fetch_view();
  auto v = view_snapshot();
  if (!v || v->generation != gen || v->revision == view_revision)
    return;
  for (size_t i = 0; i < v->images.size() && i < 9; ++i) {
    if (!pictures[i])
      continue;
    auto image = v->images[i];
    if (image) {
      auto &d = descriptors[i];
      lv_image_cache_drop(&d);
      d = {};
      d.header.magic = LV_IMAGE_HEADER_MAGIC;
      d.header.cf = LV_COLOR_FORMAT_RGB565;
      d.header.w = image->width;
      d.header.h = image->height;
      d.header.stride = image->width * 2;
      d.data_size = image->size();
      d.data = image->pixels.get();
      lv_image_set_src(pictures[i], &d);
      if (selected.empty()) {
        unsigned scale = std::max(64 * 256 / image->width, 64 * 256 / image->height);
        lv_image_set_scale(pictures[i], scale);
      }
      lv_obj_remove_flag(pictures[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_center(pictures[i]);
      if (index >= 0 && i == 0) {
        if (position) {
          char s[80];
          auto *a = current();
          snprintf(s, sizeof s, "%d / %u · 左右滑动", index + 1,
                   unsigned(a ? a->photos.size() : 0));
          lv_label_set_text(position, s);
        }
        if (slide_direction) {
          lv_anim_t anim;
          lv_anim_init(&anim);
          lv_anim_set_var(&anim, pictures[i]);
          lv_anim_set_exec_cb(
              &anim, [](void *p, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t *>(p), x); });
          int end = lv_obj_get_x(pictures[i]);
          lv_anim_set_values(&anim, end + slide_direction * 432, end);
          lv_anim_set_duration(&anim, 160);
          lv_anim_start(&anim);
          slide_direction = 0;
        }
      }
      if (labels[i])
        lv_obj_add_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
    } else if (labels[i] && !v->errors[i].empty()) {
      lv_obj_add_flag(pictures[i], LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(labels[i], LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(labels[i], v->errors[i].c_str());
    }
  }
  frame = v;
  view_revision = v->revision;
}
void create(lv_obj_t *parent) {
  root = lv_obj_create(parent);
  plain(root);
  lv_obj_set_size(root, 432, 384);
  body = lv_obj_create(root);
  plain(body);
  lv_obj_set_size(body, 432, 384);
  selected.clear();
  page = 0;
  index = -1;
  shown = snapshot();
  draw();
}
void enter() { timer = lv_timer_create(update, 150, nullptr); }
void leave() {
  if (timer) {
    lv_timer_delete(timer);
    timer = nullptr;
  }
  cancel_view();
}
void destroy() {
  leave();
  if (body)
    lv_obj_clean(body);
  clear_images();
  shown.reset();
  root = body = nullptr;
}
bool back() {
  if (selected.empty())
    return false;
  if (index >= 0) {
    page = index / 9;
    index = -1;
  } else
    selected.clear();
  draw();
  return true;
}
} // namespace
const App app = {"gallery", "相册", LV_SYMBOL_IMAGE, 0, create, enter, leave, destroy, back};
} // namespace ya::gallery
