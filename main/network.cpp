#include "notifications.hpp"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_rrm.h"
#include "esp_wnm.h"
#include "esp_heap_caps.h"
#include "ya.hpp"
#include <algorithm>
#include <new>
namespace ya {
namespace {
using wifi_policy::digest;
using wifi_policy::Profile;
using wifi_policy::Record;
struct EventData {
  int kind = 0, reason = 0;
  esp_netif_ip_info_t ip{};
  uint16_t length = 0;
  uint8_t report[512]{};
};
struct Work {
  Record record, backup;
  wifi_ap_record_t raw[64]{};
  wifi_policy::Network networks[16]{};
  Profile candidate;
  char neighbors[512]{};
};
Work *work;
Record &records() { return work->record; }
StaticQueue_t event_queue_control;
QueueHandle_t events;
SemaphoreHandle_t mutex;
std::atomic<bool> runtime_allowed{true}, foreground{false};
bool started = false, scanning = false, attempt = false, manual = false,
     suppress_disconnect = false;
bool roaming = false, waiting_report = false;
WifiPhase phase = WifiPhase::OFF;
int64_t scan_deadline = 0, deadline = 0, retry_at = 0, rssi_at = 0, weak_since = -1,
        last_roam = -60000, report_until = 0;
uint32_t failed = 0, tried = 0, current_id = 0, restore_id = 0, replace_id = 0;
uint32_t scan_version = 0;
bool loaded = false;
Profile *find(uint32_t id) {
  for (auto &p : records().profiles)
    if (p.id == id && id)
      return &p;
  return nullptr;
}
int index(uint32_t id) {
  for (int i = 0; i < 8; ++i)
    if (records().profiles[i].id == id && id)
      return i;
  return -1;
}
uint64_t legacy_digest() {
  auto c = config();
  return digest(c.ssid, c.password);
}
Profile *mirror_profile() { return find(records().mirror_id); }
void publish_state(const char *message = nullptr, bool announce = false) {
  {
    Guard g(state_mutex);
    state.wifi_busy = scanning || attempt || roaming;
    auto &s = state.wlan;
    s.phase = phase;
    s.current_id = current_id;
    s.saved_count = 0;
    for (int i = 0; i < 8; ++i) {
      auto &p = records().profiles[i];
      if (!p.id)
        continue;
      auto &v = s.saved[s.saved_count++];
      v.id = p.id;
      strlcpy(v.ssid, p.ssid, sizeof v.ssid);
      v.security = p.security;
      v.automatic = p.automatic;
      v.hidden = p.hidden;
      v.auth_failed = failed & (1u << i);
    }
    if (message)
      strlcpy(state.wifi_note, message, sizeof state.wifi_note);
  }
  if (announce && message)
    notice(message);
  publish(WIFI);
}
void disconnected() {
  Guard g(state_mutex);
  state.wifi_connected = false;
  state.wifi_ssid[0] = state.ip[0] = 0;
  state.wifi_rssi = 0;
  state.wlan.signal_at = 0;
  state.wlan.k = state.wlan.v = false;
}
WifiSecurity security(wifi_auth_mode_t a) {
  switch (a) {
  case WIFI_AUTH_OPEN:
    return WifiSecurity::OPEN;
  case WIFI_AUTH_WPA2_PSK:
  case WIFI_AUTH_WPA_PSK:
  case WIFI_AUTH_WPA_WPA2_PSK:
  case WIFI_AUTH_WPA2_WPA3_PSK:
    return WifiSecurity::PERSONAL;
  case WIFI_AUTH_WPA3_PSK:
    return WifiSecurity::WPA3;
  default:
    return WifiSecurity::UNSUPPORTED;
  }
}
esp_err_t finish_commit() {
  auto &r = records();
  auto c = config();
  auto *p = mirror_profile();
  strlcpy(c.ssid, p ? p->ssid : "", sizeof c.ssid);
  strlcpy(c.password, p ? p->password : "", sizeof c.password);
  auto e = save_config(c);
  if (e != ESP_OK)
    return e;
  r.pending = 0;
  e = wifi_store_write(&r, sizeof r);
  if (e != ESP_OK)
    r.pending = 1;
  return e;
}
esp_err_t commit(uint32_t mirror_id) {
  auto &r = records();
  r.previous = legacy_digest();
  r.mirror_id = mirror_id;
  auto *p = find(mirror_id);
  r.mirror = digest(p ? p->ssid : "", p ? p->password : "");
  r.pending = 1;
  auto e = wifi_store_write(&r, sizeof r);
  if (e != ESP_OK) {
    r = work->backup;
    return e;
  }
  e = finish_commit();
  {
    Guard g(state_mutex);
    ++state.wlan.profile_version;
  }
  return e;
}
void load() {
  size_t n = sizeof(Record);
  auto e = wifi_store_read(&records(), &n);
  bool missing = e == ESP_ERR_NVS_NOT_FOUND;
  if (!missing && (e != ESP_OK || n != sizeof(Record) || !wifi_policy::valid_record(records()))) {
    notice("网络档案读取失败");
    return;
  }
  if (!missing &&
      wifi_policy::recovery(legacy_digest(), records()) == wifi_policy::Recovery::COMPLETE) {
    if (finish_commit() != ESP_OK) {
      notice("网络档案恢复失败");
      return;
    }
  }
  if (missing || records().mirror != legacy_digest()) {
    records() = Record{};
    auto c = config();
    if (c.ssid[0]) {
      auto &p = records().profiles[0];
      p.id = records().next_id++;
      strlcpy(p.ssid, c.ssid, sizeof p.ssid);
      strlcpy(p.password, c.password, sizeof p.password);
      p.security = c.password[0] ? WifiSecurity::PERSONAL : WifiSecurity::OPEN;
      p.order = ++records().order;
      records().count = 1;
      records().mirror_id = p.id;
    }
    records().mirror = legacy_digest();
    if (wifi_store_write(&records(), sizeof(Record)) != ESP_OK) {
      notice("网络档案保存失败");
      return;
    }
  }
  loaded = true;
  publish_state();
}
void cancel_scan() {
  if (scanning)
    esp_wifi_scan_stop();
  scanning = false;
  esp_wifi_clear_ap_list();
}
void begin(const Profile &p, bool user, uint32_t replacement = 0) {
  if (!loaded || ota_busy || !runtime_allowed)
    return;
  if (!started) {
    xQueueReset(events);
    if (esp_wifi_start() != ESP_OK) {
      publish_state("Wi-Fi 启动失败", true);
      return;
    }
    started = true;
  }
  cancel_scan();
  work->candidate = p;
  manual = user;
  replace_id = replacement;
  attempt = true;
  roaming = waiting_report = false;
  weak_since = -1;
  phase = WifiPhase::CONNECTING;
  wifi_ap_record_t ap{};
  suppress_disconnect = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
  esp_wifi_disconnect();
  disconnected();
  wifi_config_t wc{};
  memcpy(wc.sta.ssid, p.ssid, strlen(p.ssid));
  strlcpy((char *)wc.sta.password, p.password, sizeof wc.sta.password);
  wc.sta.pmf_cfg.capable = true;
  wc.sta.pmf_cfg.required = p.security == WifiSecurity::WPA3;
  wc.sta.threshold.authmode = p.security == WifiSecurity::OPEN   ? WIFI_AUTH_OPEN
                              : p.security == WifiSecurity::WPA3 ? WIFI_AUTH_WPA3_PSK
                                                                 : WIFI_AUTH_WPA2_PSK;
  wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
  wc.sta.rm_enabled = 1;
  wc.sta.btm_enabled = 1;
  wc.sta.ft_enabled = 1;
  wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  auto e = esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (e == ESP_OK)
    e = esp_wifi_connect();
  deadline = now_ms() + (user ? 20000 : 12000);
  if (e != ESP_OK)
    deadline = now_ms();
  {
    Guard g(state_mutex);
    strlcpy(state.wlan.target, p.ssid, sizeof state.wlan.target);
  }
  publish_state("正在连接网络", user);
}
void fail_attempt(const char *message, bool auth) {
  auto p = work->candidate;
  auto i = index(p.id);
  if (i >= 0) {
    tried |= 1u << i;
    if (auth && wifi_policy::same_credentials(p, records().profiles[i]))
      failed |= 1u << i;
  }
  attempt = false;
  phase = WifiPhase::RETRY;
  deadline = 0;
  retry_at = now_ms() + 1000;
  esp_wifi_disconnect();
  disconnected();
  memset(work->candidate.password, 0, sizeof work->candidate.password);
  publish_state(message, true);
  if (!config().wifi) {
    esp_wifi_stop();
    started = false;
    phase = WifiPhase::OFF;
    return;
  }
  if (manual && restore_id) {
    auto *old = find(restore_id);
    if (old && (old->id != p.id || !wifi_policy::same_credentials(*old, p))) {
      auto restore = *old;
      manual = false;
      restore_id = 0;
      begin(restore, false);
    }
  }
}
void scan_start() {
  if (scanning || attempt || roaming || !started || ota_busy || !runtime_allowed)
    return;
  wifi_scan_config_t sc{};
  sc.show_hidden = true;
  scanning = esp_wifi_scan_start(&sc, false) == ESP_OK;
  {
    Guard g(state_mutex);
    state.wlan.scan_at = now_ms();
  }
  if (scanning) {
    scan_deadline = now_ms() + 10000;
    phase = WifiPhase::SCANNING;
    publish_state("正在扫描");
  } else
    publish_state("扫描失败");
}
void scan_done(int status) {
  if (!scanning)
    return;
  scanning = false;
  uint16_t count = 64;
  auto e = status == 0 ? esp_wifi_scan_get_ap_records(&count, work->raw) : ESP_FAIL;
  esp_wifi_clear_ap_list();
  int n = 0;
  if (e == ESP_OK)
    for (int i = 0; i < count; ++i) {
      wifi_policy::Network v;
      memcpy(v.ssid, work->raw[i].ssid, 32);
      v.ssid[32] = 0;
      v.rssi = work->raw[i].rssi;
      v.security = security(work->raw[i].authmode);
      wifi_policy::insert(work->networks, n, 16, v);
    }
  wifi_policy::sort(work->networks, n);
  {
    Guard g(state_mutex);
    state.ap_count = n;
    state.wlan.nearby_count = n;
    state.wlan.scan_version = ++scan_version;
    state.wlan.scan_at = now_ms();
    for (int i = 0; i < n; ++i) {
      auto &v = state.wlan.nearby[i];
      static_cast<wifi_policy::Network &>(v) = work->networks[i];
      v.saved_id = 0;
      for (auto &p : records().profiles)
        if (p.id && wifi_policy::compatible(p.security, v.security) && !strcmp(p.ssid, v.ssid))
          v.saved_id = p.id;
      strlcpy(state.aps[i], v.ssid, 33);
    }
    phase = state.wifi_connected ? WifiPhase::CONNECTED : WifiPhase::IDLE;
  }
  bool online;
  {
    Guard g(state_mutex);
    online = state.wifi_connected;
  }
  publish_state(e == ESP_OK ? (online ? "已连接" : n ? "请选择网络" : "未发现网络") : "扫描失败");
  retry_at = now_ms();
}
void auto_connect() {
  if (!loaded || !started || attempt || scanning || roaming || now_ms() < retry_at)
    return;
  {
    Guard g(state_mutex);
    if (state.wifi_connected)
      return;
  }
  bool any = false;
  for (auto &p : records().profiles)
    any |= p.id && p.automatic;
  if (!any)
    return;
  int64_t scanned;
  int count;
  {
    Guard g(state_mutex);
    scanned = state.wlan.scan_at;
    count = state.wlan.nearby_count;
  }
  if (now_ms() - scanned >= 10000) {
    scan_start();
    return;
  }
  int chosen = wifi_policy::choose(records(), work->networks, count, failed | tried);
  if (chosen >= 0) {
    begin(records().profiles[chosen], false);
    return;
  }
  retry_at = now_ms() + 10000;
  tried = 0;
}
void sample_ap() {
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    return;
  {
    Guard g(state_mutex);
    state.wifi_rssi = ap.rssi;
    state.wlan.signal_at = now_ms();
    state.wlan.security = security(ap.authmode);
    state.wlan.k = esp_rrm_is_rrm_supported_connection();
    state.wlan.v = esp_wnm_is_btm_supported_connection();
  }
  if (ap.rssi < -75) {
    if (weak_since < 0)
      weak_since = now_ms();
  } else
    weak_since = -1;
}
void complete_connection(const esp_netif_ip_info_t &ip) {
  wifi_ap_record_t ap{};
  if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK)
    return;
  if (attempt && strncmp((char *)ap.ssid, work->candidate.ssid, 32))
    return;
  bool saved = true;
  if (attempt) {
    if (manual && !config().wifi) {
      auto c = config();
      c.wifi = true;
      if (save_config(c) != ESP_OK)
        saved = false;
    }
    work->backup = records();
    auto &p = work->candidate;
    int slot = index(p.id);
    if (slot < 0) {
      slot = index(replace_id);
      if (slot < 0)
        for (int i = 0; i < 8; ++i)
          if (!records().profiles[i].id) {
            slot = i;
            break;
          }
    }
    if (slot >= 0) {
      bool fresh = !p.id;
      if (fresh)
        p.id = records().next_id++;
      if (!records().profiles[slot].id)
        ++records().count;
      records().profiles[slot] = p;
      // Only update MRU when selection changes; normal reconnects do not wear NVS.
      if (records().mirror_id != p.id || manual || fresh) {
        records().profiles[slot].order = ++records().order;
        saved = (commit(p.id) == ESP_OK) && saved;
      }
      current_id = p.id;
      failed &= ~(1u << slot);
    } else
      saved = false;
  }
  attempt = roaming = false;
  waiting_report = false;
  phase = WifiPhase::CONNECTED;
  tried = 0;
  retry_at = 0;
  weak_since = -1;
  {
    Guard g(state_mutex);
    state.wifi_connected = true;
    memcpy(state.wifi_ssid, ap.ssid, 32);
    state.wifi_ssid[32] = 0;
    snprintf(state.ip, sizeof state.ip, IPSTR, IP2STR(&ip.ip));
    state.wlan.target[0] = 0;
  }
  memset(work->candidate.password, 0, sizeof work->candidate.password);
  sample_ap();
  notifications_add("Wi-Fi 已连接", NoticeKind::WIFI);
  publish_state(saved ? "已连接" : "已连接，网络保存失败", !saved);
}
void btm_query() {
  waiting_report = false;
  if (esp_wnm_is_btm_supported_connection())
    esp_wnm_send_bss_transition_mgmt_query(REASON_RSSI,
                                           work->neighbors[0] ? work->neighbors : nullptr, 0);
  work->neighbors[0] = 0;
}
void handler(void *, esp_event_base_t base, int32_t id, void *data) {
  EventData e;
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    e.kind = 1;
    e.ip = ((ip_event_got_ip_t *)data)->ip_info;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    e.kind = 2;
    e.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
    e.kind = 3;
    e.reason = ((wifi_event_sta_scan_done_t *)data)->status;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    e.kind = 4;
  else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_NEIGHBOR_REP) {
    e.kind = 5;
    if (data) {
      auto *r = (wifi_event_neighbor_report_t *)data;
      e.length = std::min<size_t>(r->report_len, sizeof e.report);
      memcpy(e.report, r->n_report, e.length);
    }
  } else
    return;
  xQueueSend(events, &e, 0); // Bounded callbacks: never wait for storage, GUI or network locks.
}
} // namespace
const char *wifi_security_name(WifiSecurity s) {
  switch (s) {
  case WifiSecurity::OPEN:
    return "开放网络";
  case WifiSecurity::PERSONAL:
    return "WPA2 / WPA3 个人";
  case WifiSecurity::WPA3:
    return "WPA3 个人";
  default:
    return "不支持的安全类型";
  }
}
void wifi_init() {
  mutex = xSemaphoreCreateMutex();
  work = (Work *)heap_caps_calloc(1, sizeof(Work), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  auto *buffer =
      (uint8_t *)heap_caps_malloc(8 * sizeof(EventData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mutex || !work || !buffer)
    abort();
  new (work) Work{};
  events = xQueueCreateStatic(8, sizeof(EventData), buffer, &event_queue_control);
  if (!events)
    abort();
  {
    Guard m(mutation_mutex);
    load();
  }
  ESP_ERROR_CHECK(esp_netif_init());
  if (!esp_netif_create_default_wifi_sta())
    abort();
  wifi_init_config_t w = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&w));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, handler, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, handler, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  wifi_apply();
}
void wifi_apply() {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  if (ota_busy)
    return;
  auto c = config();
  if (c.wifi && runtime_allowed) {
    if (!started) {
      xQueueReset(events);
      if (esp_wifi_start() != ESP_OK) {
        publish_state("Wi-Fi 启动失败", true);
        return;
      }
      started = true;
      phase = WifiPhase::IDLE;
      retry_at = 0;
      scan_start();
    }
  } else {
    attempt = roaming = waiting_report = false;
    cancel_scan();
    if (started)
      esp_wifi_stop();
    started = false;
    phase = WifiPhase::OFF;
    current_id = 0;
    memset(work->candidate.password, 0, sizeof work->candidate.password);
    disconnected();
    publish_state("Wi-Fi 已关闭");
  }
}
void wifi_scan() {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  scan_start();
}
void wifi_connect_request(const char *ssid, const char *password, int options) {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  if (ota_busy || !runtime_allowed) {
    notice("请先打开 Wi-Fi，升级时不可切换");
    return;
  }
  if (!ssid || !password || !ssid[0] || strlen(ssid) > 32 || strlen(password) > 63) {
    notice("Wi-Fi 参数无效");
    return;
  }
  Profile p;
  strlcpy(p.ssid, ssid, sizeof p.ssid);
  strlcpy(p.password, password, sizeof p.password);
  p.security = WifiSecurity(options & 3);
  if (!options && password[0])
    p.security = WifiSecurity::PERSONAL;
  p.hidden = bool(options & 8);
  if (p.security == WifiSecurity::UNSUPPORTED ||
      (p.security != WifiSecurity::OPEN && strlen(password) < 8)) {
    notice("密码至少需要 8 个字符");
    return;
  }
  if (p.security == WifiSecurity::OPEN && password[0]) {
    notice("开放网络不需要密码");
    return;
  }
  for (auto &v : records().profiles)
    if (v.id && v.security == p.security && !strcmp(v.ssid, ssid)) {
      p.id = v.id;
      p.automatic = v.automatic;
    }
  uint32_t replacement = uint32_t(options) >> 8;
  if (!p.id && records().count == 8 && !find(replacement)) {
    notice("已保存 8 个网络，请先选择替换项");
    return;
  }
  restore_id = current_id;
  begin(p, true, replacement);
}
void wifi_connect(const char *s, const char *p) { wifi_connect_request(s, p, 0); }
void wifi_connect_saved(uint32_t id) {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  auto *p = find(id);
  if (!p || ota_busy)
    return;
  failed &= ~(1u << index(id));
  restore_id = current_id;
  begin(*p, true);
}
void wifi_forget_id(uint32_t id) {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  if (ota_busy)
    return;
  auto *p = find(id);
  if (!p)
    return;
  work->backup = records();
  *p = Profile{};
  --records().count;
  uint32_t mirror = records().mirror_id;
  if (mirror == id) {
    mirror = 0;
    for (auto &v : records().profiles)
      if (v.id && (!mirror || v.order > find(mirror)->order))
        mirror = v.id;
  }
  if (commit(mirror) != ESP_OK) {
    notice("忘记网络保存失败，请重试");
    publish_state();
    return;
  }
  if (current_id == id || (attempt && work->candidate.id == id)) {
    attempt = roaming = false;
    esp_wifi_disconnect();
    disconnected();
    current_id = 0;
    phase = WifiPhase::IDLE;
    retry_at = now_ms() + 1000;
  }
  failed = tried = 0;
  publish_state("已忘记网络", true);
}
void wifi_forget() {
  uint32_t id;
  {
    Guard g(state_mutex);
    id = state.wlan.current_id;
  }
  if (!id)
    id = records().mirror_id;
  wifi_forget_id(id);
}
void wifi_profile_auto(uint32_t id, bool enabled) {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  auto *p = find(id);
  if (!p || ota_busy)
    return;
  work->backup = records();
  p->automatic = enabled;
  auto e = commit(records().mirror_id);
  publish_state(e == ESP_OK ? "设置已保存" : "设置保存失败", true);
}
void wifi_foreground(bool enabled) { foreground = enabled; }
void wifi_tick() {
  Guard lock(mutex);
  Guard m(mutation_mutex);
  if (!runtime_allowed || !started)
    return;
  bool connected_now;
  {
    Guard g(state_mutex);
    connected_now = state.wifi_connected;
  }
  // Read-only link telemetry must keep refreshing while OTA blocks network changes.
  if (connected_now && now_ms() >= rssi_at) {
    rssi_at = now_ms() + 5000;
    sample_ap();
  }
  if (ota_busy)
    return;
  EventData event;
  for (int n = 0; n < 8 && xQueueReceive(events, &event, 0) == pdTRUE; ++n) {
    if (event.kind == 1) {
      complete_connection(event.ip);
    } else if (event.kind == 2) {
      if (suppress_disconnect && event.reason == WIFI_REASON_ASSOC_LEAVE) {
        suppress_disconnect = false;
        continue;
      }
      suppress_disconnect = false;
      if (event.reason == WIFI_REASON_ROAMING) {
        roaming = true;
        phase = WifiPhase::ROAMING;
        deadline = now_ms() + 15000;
        publish_state("正在切换接入点");
        continue;
      }
      if (attempt) {
        bool auth = event.reason == WIFI_REASON_AUTH_FAIL ||
                    event.reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    event.reason == WIFI_REASON_HANDSHAKE_TIMEOUT;
        fail_attempt(auth ? "密码错误或认证失败" : "连接失败", auth);
      } else {
        roaming = false;
        phase = WifiPhase::RETRY;
        disconnected();
        retry_at = now_ms() + 3000;
        notifications_add("Wi-Fi 连接断开，正在重连", NoticeKind::WIFI);
        publish_state("连接断开，正在重连");
      }
    } else if (event.kind == 3)
      scan_done(event.reason);
    else if (event.kind == 4 && roaming) {
      esp_netif_ip_info_t ip{};
      auto *net = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
      if (net && esp_netif_get_ip_info(net, &ip) == ESP_OK && ip.ip.addr)
        complete_connection(ip);
    } else if (event.kind == 5 && waiting_report) {
      wifi_policy::neighbors(event.report, event.length, work->neighbors, sizeof work->neighbors);
      btm_query();
    }
  }
  if (scanning && now_ms() >= scan_deadline) {
    cancel_scan();
    phase = WifiPhase::IDLE;
    retry_at = now_ms() + 10000;
    publish_state("扫描超时", true);
  }
  if (attempt && now_ms() >= deadline)
    fail_attempt("连接超时，正在恢复网络", false);
  if (roaming && now_ms() >= deadline) {
    roaming = false;
    esp_wifi_disconnect();
    disconnected();
    phase = WifiPhase::RETRY;
    retry_at = now_ms() + 1000;
    publish_state("漫游超时，正在重连");
  }
  if (waiting_report && now_ms() >= report_until)
    btm_query();
  int64_t scanned;
  {
    Guard g(state_mutex);
    connected_now = state.wifi_connected;
    scanned = state.wlan.scan_at;
  }
  if (connected_now && !attempt && !scanning && !roaming &&
      wifi_policy::roam_due(now_ms(), weak_since, last_roam)) {
    last_roam = now_ms();
    work->neighbors[0] = 0;
    if (esp_wnm_is_btm_supported_connection()) {
      waiting_report =
          esp_rrm_is_rrm_supported_connection() && esp_rrm_send_neighbor_report_request() == 0;
      report_until = now_ms() + 3000;
      if (!waiting_report)
        btm_query();
    }
  }
  if (!connected_now)
    auto_connect();
  else if (foreground && !waiting_report && now_ms() - scanned >= 15000)
    scan_start();
}
void wifi_runtime(bool allowed) {
  if (runtime_allowed.exchange(allowed) != allowed)
    wifi_apply();
}
} // namespace ya
