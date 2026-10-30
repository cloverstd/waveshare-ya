#pragma once
#include "gallery_types.hpp"
#include "lvgl.h"
#include <atomic>
namespace ya {
struct App {
  const char *id, *name, *icon;
  uint32_t capabilities;
  void (*create)(lv_obj_t *);
  void (*enter)();
  void (*leave)();
  void (*destroy)();
  bool (*back)();
};
extern std::atomic<bool> screen_sleeping;
namespace gallery {
std::shared_ptr<const Catalog> snapshot();
std::shared_ptr<const View> view_snapshot();
uint32_t view(const std::string &, int, int, bool = false);
void cancel_view();
extern const App app;
} // namespace gallery
} // namespace ya
