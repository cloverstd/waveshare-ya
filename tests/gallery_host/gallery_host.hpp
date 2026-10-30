#pragma once
#include "gallery_types.hpp"
#include "cJSON.h"
#include <atomic>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <map>
#include <cstdio>
#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/sha.h>
#endif
namespace host {
inline std::string root;
inline std::map<std::string, std::string> roots;
inline bool fail_commit = false;
inline size_t capacity = 3 * 1024 * 1024;
} // namespace host
#define YA_GALLERY_FS_ROOT host::root.c_str()
using SemaphoreHandle_t = void *;
using QueueHandle_t = void *;
using StackType_t = uint8_t;
struct StaticTask_t {};
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_NVS_NOT_FOUND = 1, pdTRUE = 1;
#define pdMS_TO_TICKS(x) (x)
inline void *xSemaphoreCreateMutex() { return (void *)1; }
inline void *xQueueCreate(int, size_t) { return (void *)1; }
inline int xQueueReceive(void *, void *, int) { return 0; }
inline int xQueueSend(void *, void *, int) { return 1; }
inline unsigned uxQueueMessagesWaiting(void *) { return 0; }
inline void *xTaskCreateStatic(void (*)(void *), const char *, size_t, void *, int, StackType_t *,
                               StaticTask_t *) {
  return (void *)1;
}
namespace ya {
struct Guard {
  explicit Guard(void *) {}
};
inline void *state_mutex = nullptr, *storage_mutex = nullptr, *mutation_mutex = nullptr;
inline std::atomic<bool> ota_busy{false}, power_paused{false}, screen_sleeping{false};
inline struct {
  bool flash_mounted = true, wifi_connected = false;
} state;
inline int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
inline void power_hold(unsigned) {}
inline esp_err_t app_config_get_slot(const char *slot, const char *, unsigned, void *out,
                                     size_t *size) {
  auto it = host::roots.find(slot);
  if (it == host::roots.end())
    return ESP_ERR_NVS_NOT_FOUND;
  if (*size < it->second.size() + 1)
    return 2;
  *size = it->second.size() + 1;
  memcpy(out, it->second.c_str(), *size);
  return ESP_OK;
}
inline esp_err_t app_config_get(const char *key, unsigned v, void *out, size_t *size) {
  return app_config_get_slot("ota_0", key, v, out, size);
}
inline esp_err_t app_config_set(const char *, unsigned, const void *data, size_t) {
  if (host::fail_commit)
    return 2;
  host::roots["ota_0"] = static_cast<const char *>(data);
  return ESP_OK;
}
inline int storage_list(const char *, size_t, size_t, void (*)(const char *, void *), void *) {
  return 2;
}
inline int storage_read(const char *, size_t, void *, size_t, size_t *) { return 2; }
} // namespace ya
inline int esp_spiffs_info(const char *, size_t *total, size_t *used) {
  *total = host::capacity;
  *used = 0;
  for (auto &p : std::filesystem::directory_iterator(host::root))
    *used += p.file_size();
  return 0;
}
inline unsigned esp_random() {
  static unsigned r = 123;
  return ++r;
}
inline int mbedtls_sha256(const uint8_t *data, size_t n, uint8_t *out, int) {
#ifdef __APPLE__
  CC_SHA256(data, n, out);
#else
  SHA256(data, n, out);
#endif
  return 0;
}
inline int mbedtls_base64_decode(uint8_t *out, size_t size, size_t *n, const uint8_t *in,
                                 size_t len) {
  *n = 0;
  unsigned acc = 0, bits = 0;
  for (size_t i = 0; i < len; ++i) {
    if (in[i] == '=')
      break;
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto *p = strchr(alphabet, in[i]);
    if (!p)
      return -1;
    acc = (acc << 6) | unsigned(p - alphabet);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (*n >= size)
        return -1;
      out[(*n)++] = (acc >> bits) & 255;
    }
  }
  return 0;
}
struct esp_http_client_event_t {
  int event_id;
  const char *header_key, *header_value;
  void *user_data;
};
constexpr int HTTP_EVENT_ON_HEADER = 1;
struct esp_http_client_config_t {
  const char *url;
  void *user_data;
  int (*event_handler)(esp_http_client_event_t *);
  int (*crt_bundle_attach)(void *);
  int timeout_ms;
  bool disable_auto_redirect;
  int buffer_size;
};
inline int esp_crt_bundle_attach(void *) { return 0; }
inline void *esp_http_client_init(esp_http_client_config_t *) { return nullptr; }
inline int esp_http_client_set_header(void *, const char *, const char *) { return 0; }
inline int esp_http_client_open(void *, int) { return -1; }
inline int64_t esp_http_client_fetch_headers(void *) { return -1; }
inline int esp_http_client_get_status_code(void *) { return 0; }
inline int esp_http_client_cleanup(void *) { return 0; }
inline int esp_http_client_read(void *, char *, size_t) { return -1; }
inline bool esp_http_client_is_complete_data_received(void *) { return false; }
namespace ya::gallery {
inline std::shared_ptr<Image> decode(const uint8_t *, size_t n, unsigned, unsigned,
                                     std::string &error) {
  if (!n) {
    error = "unsupported_image";
    return {};
  }
  auto p = std::make_shared<Image>();
  p->width = 1;
  p->height = 1;
  return p;
}
inline std::vector<uint8_t> encode(const Image &) { return {255, 216, 0, 0, 255, 217}; }
inline std::shared_ptr<Image> thumbnail(const Image &p) { return std::make_shared<Image>(p); }
} // namespace ya::gallery
