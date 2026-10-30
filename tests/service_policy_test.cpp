#include "../main/service_policy.hpp"
#include <cassert>
#include <ctime>

using ya::service_policy::Key;

int main() {
  using namespace ya::service_policy;
  assert(battery_timeout(30) && battery_timeout(60) && battery_timeout(300));
  assert(!battery_timeout(0) && !battery_timeout(600) && !battery_timeout(-1));
  assert(usb_timeout(0) && usb_timeout(60) && usb_timeout(300) && usb_timeout(600));
  assert(!usb_timeout(30) && !usb_timeout(301));

  Key held_at_boot;
  assert(!held_at_boot.sample(true, 0, 4, 8));
  assert(!held_at_boot.sample(true, 4000, 4, 8));
  assert(!held_at_boot.sample(false, 4010, 4, 8));
  assert(!held_at_boot.sample(false, 4042, 4, 8));
  assert(!held_at_boot.sample(true, 5000, 4, 8));
  assert(!held_at_boot.sample(true, 5032, 4, 8));
  assert(!held_at_boot.sample(false, 5100, 4, 8));
  assert(held_at_boot.sample(false, 5132, 4, 8) == 4);
  assert(!held_at_boot.sample(false, 5200, 4, 8));

  Key power;
  assert(!power.sample(false, 0, 4, 8));
  assert(!power.sample(false, 32, 4, 8));
  assert(!power.sample(true, 100, 4, 8));
  assert(!power.sample(false, 115, 4, 8)); // A bouncing edge is not a click.
  assert(!power.sample(false, 160, 4, 8));
  assert(!power.sample(true, 200, 4, 8));
  assert(!power.sample(true, 232, 4, 8));
  assert(!power.sample(true, 2231, 4, 8));
  assert(power.sample(true, 2232, 4, 8) == 8);
  assert(!power.sample(true, 5000, 4, 8));
  assert(!power.sample(false, 5010, 4, 8));
  assert(!power.sample(false, 5042, 4, 8)); // Long hold must not also toggle.

  Key navigation;
  navigation.sample(false, 0, 1);
  navigation.sample(false, 32, 1);
  navigation.sample(true, 100, 1);
  navigation.sample(true, 132, 1);
  assert(!navigation.sample(true, 10000, 1));
  navigation.sample(false, 10001, 1);
  assert(navigation.sample(false, 10033, 1) == 1);

  // Compare every supported date to the host's independent UTC calendar.
  for (int64_t value = 946684800; value < 4102444800LL; value += 86400) {
    time_t t = value + 23 * 3600 + 59 * 60 + 59;
    tm utc{};
    assert(gmtime_r(&t, &utc));
    uint8_t r[] = {to_bcd(utc.tm_sec),       to_bcd(utc.tm_min),   to_bcd(utc.tm_hour),
                   to_bcd(utc.tm_mday),      uint8_t(utc.tm_wday), to_bcd(utc.tm_mon + 1),
                   to_bcd(utc.tm_year - 100)};
    int64_t decoded = 0;
    assert(rtc_epoch(r, decoded) && decoded == t);
    r[0] |= 0x80;
    assert(!rtc_epoch(r, decoded)); // Oscillator-stopped time is untrusted.
  }
  uint8_t invalid[] = {0x00, 0x00, 0x00, 0x29, 4, 0x02, 0x23};
  int64_t result = 0;
  assert(!rtc_epoch(invalid, result)); // 2023 is not leap.
  invalid[6] = 0x24;
  assert(rtc_epoch(invalid, result));
  invalid[3] = 0x30;
  assert(!rtc_epoch(invalid, result));
  invalid[3] = 0x29;
  invalid[1] = 0x1a;
  assert(!rtc_epoch(invalid, result));
  invalid[1] = 0x00;
  invalid[2] = 0x24;
  assert(!rtc_epoch(invalid, result));
  invalid[2] = 0x00;
  invalid[4] = 7;
  assert(!rtc_epoch(invalid, result));
}
