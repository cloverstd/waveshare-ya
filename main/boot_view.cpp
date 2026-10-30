#include "boot_view.hpp"

namespace ya {
lv_obj_t *boot_view_create(lv_obj_t *screen, const lv_font_t *font) {
  // Cover the entire panel, including the area outside the rounded root.
  lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  auto *root = lv_obj_create(screen);
  lv_obj_remove_style_all(root);
  lv_obj_set_size(root, 480, 480);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_style_bg_color(root, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(root, 64, 0);
  lv_obj_set_style_clip_corner(root, true, 0);
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

  auto *spinner = lv_spinner_create(root);
  lv_obj_set_size(spinner, 120, 120);
  lv_obj_align(spinner, LV_ALIGN_CENTER, 0, -24);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x142b2a), LV_PART_MAIN);
  lv_obj_set_style_arc_color(spinner, lv_color_hex(0x79d5c8), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(spinner, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(spinner, 4, LV_PART_INDICATOR);
  lv_spinner_set_anim_params(spinner, 1100, 220);

  auto *brand = lv_label_create(root);
  lv_label_set_text(brand, "YA");
  lv_obj_set_style_text_font(brand, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(brand, lv_color_hex(0xe5ebf2), 0);
  lv_obj_align(brand, LV_ALIGN_CENTER, 0, -24);

  auto *loading = lv_label_create(root);
  lv_label_set_text(loading, "正在启动…");
  lv_obj_set_style_text_font(loading, font, 0);
  lv_obj_set_style_text_color(loading, lv_color_hex(0x9baab8), 0);
  lv_obj_align(loading, LV_ALIGN_CENTER, 0, 76);
  return root;
}
} // namespace ya
