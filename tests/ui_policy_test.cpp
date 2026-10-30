#include "../main/ui_policy.hpp"
#include <cassert>
#include <initializer_list>
#include <cstring>
int main() {
  using ya::ui_policy::format_bytes;
  char size[32];
  format_bytes(size, sizeof size, 0); assert(!strcmp(size, "0 B"));
  format_bytes(size, sizeof size, 1023); assert(!strcmp(size, "1023 B"));
  format_bytes(size, sizeof size, 1024); assert(!strcmp(size, "1.00 KB"));
  format_bytes(size, sizeof size, 114845); assert(!strcmp(size, "112.15 KB"));
  format_bytes(size, sizeof size, 2232320); assert(!strcmp(size, "2.13 MB"));
  format_bytes(size, sizeof size, UINT64_MAX); assert(!strcmp(size, "16.00 EB"));
  char short_size[3] = {'x', 'x', 'x'};
  format_bytes(short_size, sizeof short_size, UINT64_MAX);
  assert(short_size[2] == 0);
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
