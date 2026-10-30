#include "../main/ui_policy.hpp"
#include <cassert>
#include <initializer_list>
int main() {
  using ya::ui_policy::back_page;
  assert(back_page(0, false) == 0);
  for (int page : {1, 2, 11, 13, 14, 20})
    assert(back_page(page, true) == 0);
  for (int page : {3, 4, 5, 6, 7, 8, 9, 10, 12})
    assert(back_page(page, false) == 1);
  using ya::ui_policy::battery_color;
  assert(battery_color(true, 0) == 0xff7575);
  assert(battery_color(true, 9) == 0xff7575);
  assert(battery_color(true, 10) == 0xffd166);
  assert(battery_color(true, 50) == 0xffd166);
  assert(battery_color(true, 51) == 0x71d7a4);
  assert(battery_color(true, 100) == 0x71d7a4);
  assert(battery_color(false, 99) == 0x596779);
  assert(battery_color(true, -1) == 0x596779);
  assert(battery_color(true, 101) == 0x596779);
}
