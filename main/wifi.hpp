#pragma once
#include "wifi_policy.hpp"
#include "esp_err.h"
namespace ya {
using WifiSecurity = wifi_policy::Security;
struct WifiNetwork : wifi_policy::Network {
  uint32_t saved_id = 0;
};
struct WifiSaved {
  uint32_t id = 0;
  char ssid[33]{};
  WifiSecurity security = WifiSecurity::OPEN;
  bool automatic = true, hidden = false, auth_failed = false;
};
enum class WifiPhase : uint8_t { OFF, IDLE, SCANNING, CONNECTING, CONNECTED, ROAMING, RETRY };
struct WifiState {
  WifiNetwork nearby[16]{};
  WifiSaved saved[8]{};
  uint8_t nearby_count = 0, saved_count = 0;
  uint32_t scan_version = 0, profile_version = 0, current_id = 0;
  WifiPhase phase = WifiPhase::OFF;
  WifiSecurity security = WifiSecurity::OPEN;
  char target[33]{};
  int64_t signal_at = 0, scan_at = 0;
  bool k = false, v = false;
};
// Storage called from the background network owner under mutation_mutex.
esp_err_t wifi_store_read(void *data, size_t *size);
esp_err_t wifi_store_write(const void *data, size_t size);
void wifi_connect_saved(uint32_t id);
void wifi_profile_auto(uint32_t id, bool enabled);
void wifi_forget_id(uint32_t id);
void wifi_connect_request(const char *ssid, const char *password, int options);
void wifi_foreground(bool enabled);
void wifi_ui_preview(int view, int id);
const char *wifi_security_name(WifiSecurity);
} // namespace ya
