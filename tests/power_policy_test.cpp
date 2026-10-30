#include "../main/power_policy.hpp"
#include <cassert>
using namespace ya::power_policy;
int main() {
  // Active use must allow manual connections and automatic retries on either
  // power source, including battery staged sleep with no data request pending.
  for (unsigned mode = 0; mode <= 3; ++mode) {
    assert(keep_radios_awake(false, mode, false, false));
    assert(keep_radios_awake(true, mode, true, false)); // OTA / boot verification
    assert(keep_radios_awake(true, mode, false, true)); // physical wake
  }
  // A switch to screen-only (e.g. USB insertion) releases runtime suspension.
  assert(keep_radios_awake(true, 0, false, false));
  // Actual light/deep/staged sleep retains its existing radio/sync policy.
  for (unsigned mode = 1; mode <= 3; ++mode)
    assert(!keep_radios_awake(true, mode, false, false));
  assert(!deep_due(3, 599999, 10, false, false));
  assert(deep_due(3, 600000, 10, false, false));
  assert(!deep_due(3, 600001, 10, false, true));
  assert(deep_due(2, 0, 10, false, false));
  assert(!deep_due(1, 99999999, 10, false, false));
  assert(!deep_due(0, 99999999, 10, false, false));
  assert(deep_due(3, 0, 10, true, false));
  assert(screen_only_window(599999, 10, false));
  assert(!screen_only_window(600000, 10, false));
  assert(!screen_only_window(0, 10, true));
  assert(!screen_only_window(0, 0, false));
  assert(!deep_due(3, 600000 - 600000, 10, false, false));
  assert(deep_due(3, 1200000 - 600000, 10, false, false));
  assert(restore_deadline(100, 10000000, 4000000) == 6100);
  assert(restore_deadline(100, 4000000, 10000000) == 100);
}
