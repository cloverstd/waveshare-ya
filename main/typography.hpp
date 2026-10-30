#pragma once
#include "lvgl.h"

extern "C" {
extern const lv_font_t font_zh, font_zh_caption, font_zh_title;
extern const lv_font_t font_inter_20, font_inter_24, font_inter_28, font_inter_40;
}

namespace ya::typography {
// Design units map to native pixels, without scaling a low-resolution bitmap.
// LVGL continues to draw at 480x480; positions and touch coordinates stay native.
constexpr int scale = 2;
constexpr int px(int units) { return units * scale; }
inline const lv_font_t *const body = &font_zh;             // 12 units, 24px, Regular
inline const lv_font_t *const caption = &font_zh_caption;  // 10 units, 20px, Regular
inline const lv_font_t *const title = &font_zh_title;      // 14 units, 28px, Medium
inline const lv_font_t *const number_small = &font_inter_20;
inline const lv_font_t *const clock = &font_inter_24;
inline const lv_font_t *const brand = &font_inter_28;
inline const lv_font_t *const number_large = &font_inter_40;
} // namespace ya::typography
