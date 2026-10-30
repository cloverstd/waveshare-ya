#pragma once
#include "ya.hpp"
#include <string>
namespace ya {
struct PowerProfile {
  uint8_t mode = 3, ble_receive = 1, route = 0;
  uint16_t light_minutes = 10, sync_minutes = 15;
};
struct PowerPrefs {
  PowerProfile battery, usb{0, 1, 0, 10, 15};
};
extern std::atomic<bool> screen_sleeping, power_paused, power_wake_ui;
void power_init();
void power_notify();
bool power_notification_active();
void power_hold(unsigned milliseconds);
void power_tick(); // main task, never under GUI lock
bool power_silent_boot();
PowerProfile power_profile();
int power_value(const char *key);
esp_err_t power_setting(const char *key, int value);
std::string power_diagnostics();
} // namespace ya
