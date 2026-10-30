#pragma once
#include "ya.hpp"
#include <string>
namespace ya {
struct DataView {
  std::string resource, params, json, error;
  bool fresh = false;
};
void data_init();
std::string data_identity();
// Nonblocking application API. JSON is a logical request, never a transport handle.
bool data_request(const char *resource, const char *params = "{}", const char *op = "get",
                  const char *payload = "{}", const char *idempotency_key = "");
bool data_subscribe(const char *resource, const char *params, unsigned seconds = 60);
void data_unsubscribe(const char *resource, const char *params);
DataView data_view(const char *resource, const char *params = "{}");
bool data_configure(const char *json); // validated/persisted asynchronously
std::string data_diagnostics();
std::string data_summary();
bool data_busy();
bool data_has_plans();
bool data_needs_wifi();
void data_sync();
bool data_checkpoint();
void data_link(int conn, bool connected);
bool data_receive(int conn, const uint8_t *, size_t);
void data_subscribed(int conn, bool enabled);
// BLE adapter, uses the existing NimBLE host only.
bool ble_data_send(int conn, const uint8_t *, size_t);
int ble_data_rssi(int conn);
void ble_data_drop(int conn, const char *reason = "unspecified");
void ble_allow_candidates(bool allowed);
void data_cancel_round();
void wifi_runtime(bool allowed);
void ble_runtime(bool allowed);
} // namespace ya
