#include "../main/service_policy.hpp"
#include <cassert>
#include <cstdint>
#include <initializer_list>

using ya::service_policy::Key;
using ya::service_policy::KEY_WAKE;

int main() {
  // The GUI is stopped and the power task may not run until t=1000. Vary the
  // press phase: every 96 ms press ends before that old one-second GPIO poll.
  for (uint32_t click : {1u, 2u, 4u}) {
    for (bool immediate_resume : {false, true}) {
      for (int start = 80; start <= 880; start += 16) {
        Key key;
        bool sleeping = true;
        unsigned wakes = 0, actions = 0;
        uint32_t pending = 0;
        for (int t = 0; t <= 1104; t += 16) {
          bool down = t >= start && t < start + 96;
          auto event = key.sample(down, t, click, click == 4 ? 8 : 0,
                                  sleeping ? KEY_WAKE : 0);
          if (event & KEY_WAKE) {
            ++wakes;
            assert(t <= start + 48);
            pending |= event;
            if (immediate_resume)
              sleeping = false;
          }
          actions |= event & ~KEY_WAKE;
          if (t == 992) {
            assert(!down && (pending & KEY_WAKE));
            sleeping = false;
          }
        }
        assert(wakes == 1 && actions == 0); // release cannot toggle/navigate
        // A fresh, deliberate press after wake still has its normal action.
        assert(!key.sample(true, 1200, click));
        assert(!key.sample(true, 1232, click));
        assert(!key.sample(false, 1300, click));
        assert(key.sample(false, 1332, click) == click);
      }
    }
  }

  // Wake on press must retain the existing two-second orderly shutdown hold.
  Key power;
  power.sample(false, 0, 4, 8, KEY_WAKE);
  power.sample(false, 32, 4, 8, KEY_WAKE);
  assert(!power.sample(true, 100, 4, 8, KEY_WAKE));
  assert(power.sample(true, 132, 4, 8, KEY_WAKE) == KEY_WAKE);
  assert(!power.sample(true, 2131, 4, 8));
  assert(power.sample(true, 2132, 4, 8) == 8);
  assert(!power.sample(true, 5000, 4, 8));
  assert(!power.sample(false, 5010, 4, 8));
  assert(!power.sample(false, 5042, 4, 8));

  // Contact bounce does not wake; a key held on boot must first be released.
  Key bounce;
  bounce.sample(false, 0, 4, 8, KEY_WAKE);
  bounce.sample(false, 32, 4, 8, KEY_WAKE);
  assert(!bounce.sample(true, 100, 4, 8, KEY_WAKE));
  assert(!bounce.sample(false, 116, 4, 8, KEY_WAKE));
  assert(!bounce.sample(false, 148, 4, 8, KEY_WAKE));
  Key boot;
  assert(!boot.sample(true, 0, 4, 8, KEY_WAKE));
  assert(!boot.sample(true, 5000, 4, 8, KEY_WAKE));
  assert(!boot.sample(false, 5010, 4, 8, KEY_WAKE));
  assert(!boot.sample(false, 5042, 4, 8, KEY_WAKE));
  assert(!boot.sample(true, 5100, 4, 8, KEY_WAKE));
  assert(boot.sample(true, 5132, 4, 8, KEY_WAKE) == KEY_WAKE);

  // Auto screen-off can fall between a press and its release. That release
  // must wake instead of producing a screen toggle or a navigation action.
  Key boundary;
  boundary.sample(false, 0, 4, 8);
  boundary.sample(false, 32, 4, 8);
  boundary.sample(true, 100, 4, 8);
  boundary.sample(true, 132, 4, 8);
  assert(!boundary.sample(false, 200, 4, 8, KEY_WAKE));
  assert(boundary.sample(false, 232, 4, 8, KEY_WAKE) == KEY_WAKE);
}
