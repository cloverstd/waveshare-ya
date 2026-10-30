#pragma once
#include "lvgl.h"

namespace ya {
// System startup overlay. Deleting the root also stops the spinner animation.
lv_obj_t *boot_view_create(lv_obj_t *screen, const lv_font_t *font);
} // namespace ya
