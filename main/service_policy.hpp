#pragma once
#include <cstddef>
#include <cstdint>

namespace ya::service_policy {
inline bool battery_timeout(int seconds) {
  return seconds == 30 || seconds == 60 || seconds == 300;
}
inline bool usb_timeout(int seconds) {
  return seconds == 0 || seconds == 60 || seconds == 300 || seconds == 600;
}

constexpr uint32_t KEY_WAKE = 16;

// Samples must be called frequently, independently of the GUI's sleep state. A key held
// during boot stays disarmed until released, including the power-on key press.
struct Key {
  bool initialized = false, raw = false, stable = false, armed = false;
  bool pressed = false, long_sent = false, wake_sent = false;
  int64_t changed = 0, down = 0;

  uint32_t sample(bool level, int64_t now, uint32_t click, uint32_t hold = 0,
                  uint32_t wake = 0) {
    if (!initialized) {
      initialized = true;
      raw = stable = level;
      changed = now;
      return 0;
    }
    if (raw != level) {
      raw = level;
      changed = now;
    }
    uint32_t event = 0;
    if (now - changed >= 32) {
      if (stable != raw) {
        stable = raw;
        if (stable && armed) {
          pressed = true;
          long_sent = false;
          wake_sent = wake != 0;
          event = wake;
          down = now;
        } else if (!stable) {
          if (armed && pressed && !long_sent && !wake_sent)
            event = wake ? wake : click;
          pressed = false;
          armed = true;
        }
      }
      if (!stable)
        armed = true;
    }
    if (hold && armed && pressed && stable && raw && !long_sent && now - down >= 2000) {
      long_sent = true;
      event = hold;
    }
    return event;
  }
};

inline bool bcd(uint8_t raw, uint8_t mask, int low, int high, int &value) {
  if (raw & ~mask)
    return false;
  if ((raw & 15) > 9 || (raw >> 4) > 9)
    return false;
  value = (raw >> 4) * 10 + (raw & 15);
  return value >= low && value <= high;
}
inline uint8_t to_bcd(int value) { return uint8_t((value / 10) * 16 + value % 10); }
inline bool leap(int year) { return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0); }
inline int month_days(int year, int month) {
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return month >= 1 && month <= 12 ? days[month - 1] + (month == 2 && leap(year)) : 0;
}
// RTC calendar is UTC, limited to its unambiguous 2000-2099 century. No TZ
// global or mktime() is involved in this conversion.
inline bool rtc_epoch(const uint8_t *r, int64_t &epoch) {
  int second, minute, hour, day, month, year;
  if (!bcd(r[0], 0x7f, 0, 59, second) || !bcd(r[1], 0x7f, 0, 59, minute) ||
      !bcd(r[2], 0x3f, 0, 23, hour) || !bcd(r[3], 0x3f, 1, 31, day) || r[4] > 6 ||
      !bcd(r[5], 0x1f, 1, 12, month) || !bcd(r[6], 0xff, 0, 99, year))
    return false;
  year += 2000;
  if (day > month_days(year, month))
    return false;
  int64_t days = 10957; // 2000-01-01 relative to 1970-01-01.
  for (int y = 2000; y < year; y++)
    days += leap(y) ? 366 : 365;
  for (int m = 1; m < month; m++)
    days += month_days(year, m);
  days += day - 1;
  if ((days + 4) % 7 != r[4])
    return false;
  epoch = days * 86400 + hour * 3600 + minute * 60 + second;
  return true;
}
} // namespace ya::service_policy
