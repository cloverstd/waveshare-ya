#include "notifications.hpp"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"
#include "identity.hpp"
#include "ota_contract.hpp"
#if CONFIG_YA_TEST_HTTPS_CA || YA_TEST_CA_ENABLE
#include "../private/test-ca.h"
#endif
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_task_wdt.h"
#include "mbedtls/sha256.h"
#include "ya.hpp"
#include <algorithm>
#include <cstdlib>
#include <strings.h>
namespace ya {
static httpd_handle_t server;
static QueueHandle_t jobs;
static void worker(void *);
static SemaphoreHandle_t ota_mutex;
static char token[33]{};
static int64_t expires = 0;
static std::atomic<bool> checking{false};
using ota_contract::https_url;
using ota_contract::Release;
static Release release;
const char *ota_phase_name(OtaPhase phase) {
  switch (phase) {
  case OtaPhase::IDLE:
    return "idle";
  case OtaPhase::CHECKING:
    return "checking";
  case OtaPhase::AVAILABLE:
    return "available";
  case OtaPhase::CURRENT:
    return "current";
  case OtaPhase::TRANSFERRING:
    return "transferring";
  case OtaPhase::VERIFYING:
    return "verifying";
  case OtaPhase::REBOOTING:
    return "rebooting";
  case OtaPhase::BOOT_VERIFY:
    return "boot_verify";
  case OtaPhase::COMPLETE:
    return "complete";
  case OtaPhase::FAILED:
    return "failed";
  case OtaPhase::ROLLED_BACK:
    return "rolled_back";
  }
  return "idle";
}
static void status(OtaPhase phase, const char *s, int p = -1, bool reset = false,
                   size_t total = 0, bool announce = true) {
  {
    Guard g(state_mutex);
    state.ota_phase = phase;
    strlcpy(state.ota_note, s, sizeof(state.ota_note));
    if (reset) {
      state.ota_progress = 0;
      state.ota_received = 0;
      state.ota_total = total;
    }
    if (p >= 0)
      state.ota_progress = p;
  }
  if (announce && (phase == OtaPhase::COMPLETE || phase == OtaPhase::FAILED || phase == OtaPhase::ROLLED_BACK))
    notifications_add(s, NoticeKind::OTA);
  publish(OTA);
}
static bool acquire() {
  Guard mutation(mutation_mutex);
  Guard l(ota_mutex);
  Guard g(state_mutex);
  const auto &b = state.battery;
  if (ota_busy || checking || state.pending || state.wifi_busy ||
      !update_power_allowed(b.valid, now_ms() - b.sampled < 5000, b.usb, b.present, b.percent))
    return false;
  ota_busy = true;
  return true;
}
static void fail(const char *s) {
  status(OtaPhase::FAILED, s);
  diag_set("ota_result", s);
  ota_busy = false;
}
static const char *write_error(esp_err_t error) {
  if (error == ESP_ERR_TIMEOUT)
    return "固件传输中断或超时，保留原固件";
  if (error == ESP_ERR_INVALID_VERSION)
    return "固件产品、板型或版本不匹配，保留原固件";
  if (error == ESP_ERR_INVALID_SIZE)
    return "固件长度不符，保留原固件";
  if (error == ESP_ERR_INVALID_CRC)
    return "固件 SHA256 校验失败，保留原固件";
  if (error == ESP_ERR_OTA_VALIDATE_FAILED)
    return "固件签名或镜像校验失败，保留原固件";
  return "固件写入或升级配置失败，保留原固件";
}
static bool auth(httpd_req_t *req) {
  char h[80]{};
  if (httpd_req_get_hdr_value_str(req, "Authorization", h, sizeof(h)) != ESP_OK)
    return false;
  Guard l(ota_mutex);
  if (!token[0] || now_ms() >= expires || strlen(h) != 39 || strncmp(h, "Bearer ", 7))
    return false;
  unsigned diff = 0;
  for (int i = 0; i < 32; i++)
    diff |= (h[i + 7] ^ token[i]);
  return diff == 0;
}
static esp_err_t error(httpd_req_t *r, const char *code, const char *msg, size_t remaining = 0) {
  // Drain a bounded rejected body before closing, otherwise TCP RST can discard the HTTP error.
  char discard[1024];
  auto until = now_ms() + 15000;
  while (remaining && remaining <= OTA_MAX && now_ms() < until) {
    int n = httpd_req_recv(r, discard, std::min(remaining, sizeof(discard)));
    if (n <= 0)
      break;
    remaining -= n;
  }
  httpd_resp_set_status(r, code);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(r, "Connection", "close");
  httpd_resp_sendstr(r, msg);
  return ESP_FAIL;
}
static const char *page =
    R"HTML(<!doctype html><meta charset="utf-8"><meta name="viewport" content="width=device-width"><title>YA 固件升级</title><style>body{background:#080b10;color:#eee;font:18px system-ui;max-width:600px;margin:50px auto;padding:24px}button,input{font:inherit;margin:12px 0;padding:12px}progress{width:100%}small{color:#aaa}</style><h1>YA 固件升级</h1><p>选择本设备的签名 OTA .bin 文件。</p><input type="file" id="f" accept=".bin"><br><button id="b">上传并升级</button><progress id="p" max="100" value="0"></progress><p id="s">请从设备设置页打开此地址（含访问凭证）。</p><small>上传中请保持供电。启动验证失败时设备自动回退。</small><script>const t=location.hash.slice(1);history.replaceState(null,'',location.pathname);b.onclick=()=>{if(!f.files[0]||!t)return;s.textContent='正在上传';p.value=0;b.disabled=true;const x=new XMLHttpRequest();x.open('POST','/api/ota');x.setRequestHeader('Authorization','Bearer '+t);x.setRequestHeader('Content-Type','application/octet-stream');x.upload.onprogress=e=>{if(e.lengthComputable){p.value=e.loaded/e.total*100;s.textContent='正在上传 '+Math.floor(p.value)+'%'}};x.upload.onload=()=>{s.textContent='传输完成，设备正在校验签名和升级配置'};x.onload=()=>{s.textContent=x.responseText;b.disabled=false};x.onerror=()=>{s.textContent='连接中断，请检查设备上的升级结果';b.disabled=false};x.send(f.files[0]);};</script>)HTML";
static esp_err_t root(httpd_req_t *r) {
  httpd_resp_set_type(r, "text/html; charset=utf-8");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  return httpd_resp_sendstr(r, page);
}
class Writer {
  esp_ota_handle_t handle = 0;
  bool active = false;
  size_t total = 0, expected;
  const esp_partition_t *target = nullptr;
  uint8_t prefix[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                 sizeof(esp_app_desc_t) + sizeof(FirmwareIdentity)]{};
  size_t filled = 0;
  int64_t last_progress = 0;
  char expected_version[32]{};
  mbedtls_sha256_context hash;

public:
  explicit Writer(size_t n, const char *version = nullptr) : expected(n) {
    if (version)
      strlcpy(expected_version, version, sizeof(expected_version));
    mbedtls_sha256_init(&hash);
    mbedtls_sha256_starts(&hash, 0);
    status(OtaPhase::TRANSFERRING, "正在接收并写入固件", 0, true, n);
    last_progress = now_ms();
  }
  ~Writer() {
    if (active)
      esp_ota_abort(handle);
    mbedtls_sha256_free(&hash);
  }
  void cancel() {
    if (active) {
      esp_ota_abort(handle);
      active = false;
    }
  }
  esp_err_t feed(const void *data, size_t n) {
    if (total + n > expected)
      return ESP_ERR_INVALID_SIZE;
    auto *bytes = (const uint8_t *)data;
    if (!active) {
      size_t take = std::min(n, sizeof(prefix) - filled);
      memcpy(prefix + filled, bytes, take);
      filled += take;
      if (filled == sizeof(prefix)) {
        esp_app_desc_t desc{};
        memcpy(&desc, prefix + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
               sizeof(desc));
        if (prefix[0] != ESP_IMAGE_HEADER_MAGIC || desc.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            strncmp(desc.project_name, PRODUCT, sizeof(desc.project_name)) ||
            !memchr(desc.version, 0, sizeof(desc.version)) ||
            (expected_version[0] && strcmp(desc.version, expected_version)))
          return ESP_ERR_INVALID_VERSION;
        FirmwareIdentity identity{};
        memcpy(&identity,
               prefix + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                   sizeof(esp_app_desc_t),
               sizeof(identity));
        if (!identity_valid(identity))
          return ESP_ERR_INVALID_VERSION;
        target = esp_ota_get_next_update_partition(nullptr);
        if (!target || expected > target->size || target->size != OTA_MAX)
          return ESP_ERR_INVALID_SIZE;
        auto e = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
        if (e != ESP_OK)
          return e;
        active = true;
        e = esp_ota_write(handle, prefix, sizeof(prefix));
        if (e != ESP_OK)
          return e;
        if (n > take) {
          e = esp_ota_write(handle, bytes + take, n - take);
          if (e != ESP_OK)
            return e;
        }
      }
    } else {
      auto e = esp_ota_write(handle, data, n);
      if (e != ESP_OK)
        return e;
    }
    mbedtls_sha256_update(&hash, bytes, n);
    total += n;
    {
      Guard g(state_mutex);
      state.ota_received = total;
      state.ota_progress = int(total * 100 / expected);
    }
    // Flash writes are streamed; publishing every 4 KiB needlessly floods the UI queue.
    const int64_t now = now_ms();
    if (now - last_progress >= 100) {
      last_progress = now;
      publish(OTA);
    }
    return ESP_OK;
  }
  esp_err_t finish(const char *sha) {
    if (!active || total != expected)
      return ESP_ERR_INVALID_SIZE;
    status(OtaPhase::VERIFYING, "传输完成，正在校验固件与签名", 100);
    uint8_t digest[32];
    mbedtls_sha256_finish(&hash, digest);
    char hex[65];
    for (int i = 0; i < 32; i++)
      sprintf(hex + 2 * i, "%02x", digest[i]);
    if (sha && strcmp(hex, sha))
      return ESP_ERR_INVALID_CRC;
    auto e = esp_ota_end(handle);
    active = false;
    if (e != ESP_OK)
      return e;
    // esp_ota_end validates flash contents, which may include stale bytes after a short upload.
    esp_image_metadata_t metadata{};
    esp_partition_pos_t position{target->address, target->size};
    e = esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata);
    if (e != ESP_OK || metadata.image_len != expected) {
      ESP_LOGW("ota", "image size mismatch: verified=%lu received=%u",
               (unsigned long)metadata.image_len, (unsigned)expected);
      return ESP_ERR_INVALID_SIZE;
    }
    status(OtaPhase::VERIFYING, "签名通过，正在准备升级配置", 100);
    e = copy_config_to(target->label);
    if (e != ESP_OK)
      return e;
    esp_app_desc_t d{};
    esp_ota_get_partition_description(target, &d);
    diag_set("ota_target", d.version);
    diag_set("ota_slot", target->label);
    diag_set("ota_result", "等待新固件启动验证");
    e = esp_ota_set_boot_partition(target);
    return e;
  }
};
static void reboot_task(void *) {
  vTaskDelay(pdMS_TO_TICKS(1500));
  notifications_checkpoint();
  esp_restart();
}
static esp_err_t upload(httpd_req_t *r) {
  if (!auth(r))
    return error(r, "403 Forbidden", "访问凭证无效或已过期", r->content_len);
  if (r->content_len < 512 || r->content_len > OTA_MAX)
    return error(r, "413 Payload Too Large", "固件大小无效", r->content_len);
  if (!acquire())
    return error(r, "409 Conflict", "正在升级、等待验证或电量不足", r->content_len);
  Writer w(r->content_len);
  char b[4096];
  size_t left = r->content_len;
  esp_err_t e = ESP_OK;
  int timeouts = 0;
  const int64_t deadline = now_ms() + 600000;
  while (left) {
    if (now_ms() >= deadline) {
      e = ESP_ERR_TIMEOUT;
      break;
    }
    int n = httpd_req_recv(r, b, std::min(left, sizeof(b)));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
      continue;
    if (n <= 0) {
      e = ESP_ERR_TIMEOUT;
      break;
    }
    timeouts = 0;
    left -= n;
    e = w.feed(b, n);
    if (e != ESP_OK)
      break;
  }
  if (e == ESP_OK)
    e = w.finish(nullptr);
  if (e != ESP_OK) {
    w.cancel();
    fail(write_error(e));
    ESP_LOGW("ota", "upload rejected: %s", esp_err_to_name(e));
    return error(r, "400 Bad Request", "固件校验失败或上传中断，原固件仍可使用", left);
  }
  status(OtaPhase::REBOOTING, "校验成功，即将重启", 100);
  httpd_resp_set_type(r, "text/plain; charset=utf-8");
  httpd_resp_sendstr(r, "上传成功，设备正在重启并验证；请查看屏幕上的最终结果。");
  if (xTaskCreate(reboot_task, "ota_reboot", 2048, nullptr, 5, nullptr) != pdPASS) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    notifications_checkpoint();
  esp_restart();
  }
  return ESP_OK;
}
static esp_err_t info(httpd_req_t *r) {
  if (!auth(r))
    return error(r, "403 Forbidden", "Forbidden");
  auto s = snapshot();
  cJSON *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "version", esp_app_get_description()->version);
  cJSON_AddStringToObject(j, "result", s.ota_note);
  cJSON_AddNumberToObject(j, "progress", s.ota_progress);
  cJSON_AddStringToObject(j, "phase", ota_phase_name(s.ota_phase));
  cJSON_AddNumberToObject(j, "received", s.ota_received);
  cJSON_AddNumberToObject(j, "total", s.ota_total);
  cJSON_AddBoolToObject(j, "pending", s.pending);
  char *str = cJSON_PrintUnformatted(j);
  httpd_resp_set_type(r, "application/json");
  httpd_resp_set_hdr(r, "Cache-Control", "no-store");
  httpd_resp_sendstr(r, str);
  free(str);
  cJSON_Delete(j);
  return ESP_OK;
}
void ota_init() {
  ota_mutex = xSemaphoreCreateMutex();
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.stack_size = 16384;
  c.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  c.recv_wait_timeout = 8;
  c.lru_purge_enable = true;
  ESP_ERROR_CHECK(httpd_start(&server, &c));
  {
    httpd_uri_t u{};
    u.uri = "/";
    u.method = HTTP_GET;
    u.handler = root;
    httpd_register_uri_handler(server, &u);
    u.uri = "/api/ota";
    u.method = HTTP_POST;
    u.handler = upload;
    httpd_register_uri_handler(server, &u);
    u.uri = "/api/status";
    u.method = HTTP_GET;
    u.handler = info;
    httpd_register_uri_handler(server, &u);
  }
  jobs = xQueueCreate(2, sizeof(int));
  if (!jobs)
    abort();
  // Long-lived flash workers need guaranteed contiguous internal RAM.
  static StackType_t stack[16384];
  static StaticTask_t task;
  if (!xTaskCreateStatic(worker, "ota_worker", sizeof stack, nullptr, 4, stack, &task))
    abort();
  char result[160];
  diag_get("ota_result", result, sizeof(result));
  if (result[0]) {
    auto phase = strstr(result, "回退")       ? OtaPhase::ROLLED_BACK
                 : strstr(result, "升级完成") ? OtaPhase::COMPLETE
                 : strstr(result, "保留原固件") || strstr(result, "失败") ||
                         strstr(result, "中断") || strstr(result, "无效")
                     ? OtaPhase::FAILED
                     : OtaPhase::IDLE;
    status(phase, result, phase == OtaPhase::COMPLETE ? 100 : 0, true, 0, false);
  }
}
void ota_session() {
  Guard g(ota_mutex);
  char ip[24];
  bool connected;
  {
    Guard state_lock(state_mutex);
    connected = state.wifi_connected;
    strlcpy(ip, state.ip, sizeof(ip));
  }
  if (!server || !connected) {
    notice("请先连接 Wi-Fi");
    return;
  }
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  for (int i = 0; i < 16; i++)
    sprintf(token + 2 * i, "%02x", r[i]);
  expires = now_ms() + 600000;
  {
    Guard state_lock(state_mutex);
    snprintf(state.ota_url, sizeof(state.ota_url), "http://%s/#%s", ip, token);
  }
  notice("网页升级已开启，有效期十分钟");
}
struct ResponseHeaders {
  char content_type[96]{};
  bool type_seen = false, encoded = false, invalid = false;
};
static esp_err_t response_header(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_HEADER || !event->user_data)
    return ESP_OK;
  auto &headers = *static_cast<ResponseHeaders *>(event->user_data);
  if (!strcasecmp(event->header_key, "Content-Type")) {
    if (headers.type_seen || strlen(event->header_value) >= sizeof(headers.content_type))
      headers.invalid = true;
    headers.type_seen = true;
    strlcpy(headers.content_type, event->header_value, sizeof(headers.content_type));
  } else if (!strcasecmp(event->header_key, "Content-Encoding") &&
             strcasecmp(event->header_value, "identity")) {
    headers.encoded = true;
  }
  return ESP_OK;
}
static bool response_type(const ResponseHeaders &headers, const char *type) {
  size_t n = strlen(type);
  return !headers.invalid && !headers.encoded && headers.type_seen &&
         !strncasecmp(headers.content_type, type, n) &&
         (!headers.content_type[n] || headers.content_type[n] == ';' ||
          headers.content_type[n] == ' ');
}
static esp_http_client_handle_t client(const char *url, ResponseHeaders &headers) {
  esp_http_client_config_t c{};
  c.url = url;
  c.event_handler = response_header;
  c.user_data = &headers;
#if CONFIG_YA_TEST_HTTPS_CA || YA_TEST_CA_ENABLE
  c.cert_pem = YA_TEST_CA;
#else
  c.crt_bundle_attach = esp_crt_bundle_attach;
#endif
  c.timeout_ms = 10000;
  c.buffer_size = 4096;
  c.disable_auto_redirect = true;
  auto h = esp_http_client_init(&c);
  if (h)
    esp_http_client_set_header(h, "Accept-Encoding", "identity");
  return h;
}
static void check_task(void *) {
  const int64_t deadline = now_ms() + 30000;
  auto c = config();
  Release next;
  bool ok = false;
  const char *failure = "检查更新失败，请检查证书、时间和网络";
  ResponseHeaders headers;
  auto h = client(c.update_url, headers);
  if (h && esp_http_client_open(h, 0) == ESP_OK) {
    auto len = esp_http_client_fetch_headers(h);
    int code = esp_http_client_get_status_code(h);
    failure = code == 404                   ? "服务器尚未发布固件，请稍后重试"
              : code == 503                 ? "更新服务暂不可用，请稍后重试"
              : (code >= 300 && code < 400) ? "更新地址发生重定向，请使用直达地址"
              : code != 200                 ? "更新服务器响应失败，请稍后重试"
                                            : "更新清单格式、长度或响应类型无效";
    if (code == 200 && len > 0 && len < 4096 && !esp_http_client_is_chunked_response(h) &&
        response_type(headers, "application/json")) {
      char body[4097];
      int total = 0, n = 0;
      while (now_ms() < deadline && total < len &&
             (n = esp_http_client_read(h, body + total, len - total)) > 0)
        total += n;
      body[total] = 0;
      if (n >= 0 && total == len && now_ms() < deadline &&
          esp_http_client_is_complete_data_received(h)) {
        ok = ota_contract::parse(body, total, next, PRODUCT, BOARD, LAYOUT, OTA_MAX);
      } else
        failure = "更新清单接收中断或超时，请重试";
    }
  }
  if (h)
    esp_http_client_cleanup(h);
  {
    Guard g(ota_mutex);
    release = ok ? next : Release{};
  }
  {
    Guard g(state_mutex);
    state.update_available = ok && strcmp(next.version, esp_app_get_description()->version);
    strlcpy(state.ota_version, ok ? next.version : "", sizeof(state.ota_version));
  }
  bool different = ok && strcmp(next.version, esp_app_get_description()->version);
  status(!ok         ? OtaPhase::FAILED
         : different ? OtaPhase::AVAILABLE
                     : OtaPhase::CURRENT,
         ok ? (different ? "发现可用版本，请确认安装" : "已是当前版本") : failure);
  checking = false;
  return;
}
void ota_check() {
  {
    Guard mutation(mutation_mutex);
    Guard l(ota_mutex);
    Guard g(state_mutex);
    if (ota_busy || checking || state.pending)
      return;
    checking = true;
    state.update_available = false;
    state.ota_version[0] = 0;
  }
  status(OtaPhase::CHECKING, "正在检查更新", 0, true);
  auto c = config();
  bool connected;
  {
    Guard g(state_mutex);
    connected = state.wifi_connected;
  }
  if (!https_url(c.update_url) || !connected) {
    checking = false;
    status(OtaPhase::FAILED, "请先设置 HTTPS 更新地址并联网");
    return;
  }
  int job = 1;
  if (xQueueSend(jobs, &job, 0) != pdTRUE) {
    checking = false;
    status(OtaPhase::FAILED, "升级任务队列繁忙，请稍后重试");
  }
}
static void install_task(void *) {
  const int64_t deadline = now_ms() + 600000;
  Release r;
  {
    Guard l(ota_mutex);
    r = release;
  }
  esp_err_t e = ESP_FAIL;
  const char *failure = "固件下载连接失败，请检查证书、时间和网络";
  {
    Writer w(r.size, r.version);
    ResponseHeaders headers;
    auto h = client(r.url, headers);
    if (h && esp_http_client_open(h, 0) == ESP_OK) {
      auto len = esp_http_client_fetch_headers(h);
      int code = esp_http_client_get_status_code(h);
      failure = code == 404                   ? "服务器没有此固件文件，请重新检查更新"
                : code == 503                 ? "固件服务暂不可用，请稍后重试"
                : (code >= 300 && code < 400) ? "固件地址发生重定向，已停止升级"
                : code != 200                 ? "固件服务器响应失败，保留原固件"
                                              : "固件响应长度、类型或编码无效，保留原固件";
      if (code == 200 && len == (int64_t)r.size && !esp_http_client_is_chunked_response(h) &&
          response_type(headers, "application/octet-stream")) {
        char b[4096];
        size_t total = 0;
        e = ESP_OK;
        while (total < r.size) {
          if (now_ms() >= deadline) {
            e = ESP_ERR_TIMEOUT;
            break;
          }
          int n = esp_http_client_read(h, b, std::min(sizeof(b), r.size - total));
          if (n <= 0) {
            e = ESP_ERR_TIMEOUT;
            break;
          }
          e = w.feed(b, n);
          if (e != ESP_OK)
            break;
          total += n;
        }
        if (e == ESP_OK && !esp_http_client_is_complete_data_received(h))
          e = ESP_ERR_INVALID_SIZE;
        if (e == ESP_OK)
          e = w.finish(r.sha);
        if (e != ESP_OK)
          failure = write_error(e);
      }
    }
    if (h)
      esp_http_client_cleanup(h);
  }
  if (e == ESP_OK) {
    status(OtaPhase::REBOOTING, "校验成功，即将重启", 100);
    vTaskDelay(pdMS_TO_TICKS(1000));
    notifications_checkpoint();
  esp_restart();
  }
  fail(failure);
  return;
}
void ota_install() {
  bool available;
  {
    Guard g(state_mutex);
    available = state.update_available;
  }
  if (checking || !available || !acquire()) {
    notice("无法升级：请检查版本、供电或当前状态");
    return;
  }
  status(OtaPhase::TRANSFERRING, "正在连接固件服务器", 0, true);
  int job = 2;
  if (xQueueSend(jobs, &job, 0) != pdTRUE)
    fail("无法创建升级任务");
}
static void worker(void *) {
  int job;
  for (;;)
    if (xQueueReceive(jobs, &job, portMAX_DELAY)) {
      if (job == 1)
        check_task(nullptr);
      else
        install_task(nullptr);
    }
}
void ota_boot_check() {
  const auto *p = esp_ota_get_running_partition();
  esp_ota_img_states_t s;
  bool pending = esp_ota_get_state_partition(p, &s) == ESP_OK && s == ESP_OTA_IMG_PENDING_VERIFY;
  {
    Guard g(state_mutex);
    state.pending = pending;
  }
  char target[32], result[160];
  diag_get("ota_slot", target, sizeof(target));
  diag_get("ota_result", result, sizeof(result));
  if (!pending) {
    if (target[0] && strcmp(target, p->label) && !strcmp(result, "等待新固件启动验证")) {
      diag_set("ota_result", "新固件启动未通过，已自动回退");
      status(OtaPhase::ROLLED_BACK, "新固件启动未通过，已自动回退", 0, true);
    }
    return;
  }
#ifndef YA_TEST_FAILURE
#define YA_TEST_FAILURE CONFIG_YA_OTA_TEST_FAILURE
#endif
#if YA_TEST_FAILURE == 1
  abort();
#elif YA_TEST_FAILURE == 2
  for (;;)
    vTaskDelay(pdMS_TO_TICKS(1000));
#endif
  status(OtaPhase::BOOT_VERIFY, "新固件正在进行三十秒启动验证", 0, true);
  int64_t start = now_ms();
  while (now_ms() - start < 30000) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Battery b;
    {
      Guard g(state_mutex);
      b = state.battery;
    }
    if (!core_ready || !b.valid || now_ms() - b.sampled > 5000 ||
        now_ms() - ui_heartbeat.load() > 5000 || now_ms() - service_heartbeat.load() > 10000) {
      diag_set("ota_result", "启动健康检查失败，已回退");
      esp_ota_mark_app_invalid_rollback_and_reboot();
      abort();
    }
    esp_task_wdt_reset();
  }
  bool bluetooth_ready;
  {
    Guard g(state_mutex);
    bluetooth_ready = state.ble_ready;
  }
  if (!bluetooth_ready) {
    diag_set("ota_result", "蓝牙服务启动失败，已回退");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    abort();
  }
  auto e = esp_ota_mark_app_valid_cancel_rollback();
  if (e == ESP_OK) {
    diag_set("ota_result", "升级完成，启动验证通过");
    status(OtaPhase::COMPLETE, "升级完成，启动验证通过", 100);
    Guard g(state_mutex);
    state.pending = false;
  }
}
} // namespace ya
