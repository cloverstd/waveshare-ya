#include "host/ble_hs_pvcy.h"
#include "esp_mac.h"
#include "notifications.hpp"
#include "cJSON.h"
#include "battery_json.hpp"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ya.hpp"
#include "data.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "power.hpp"
#include "identity.hpp"
#include <string>
extern "C" void ble_store_config_init(void);
namespace ya {
// 98f10001-8f7c-4cb1-9e50-6bf589c91010; RX 0002, TX 0003.
static const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x01, 0x00, 0xf1, 0x98);
static const ble_uuid128_t rx_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x02, 0x00, 0xf1, 0x98);
static const ble_uuid128_t tx_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x03, 0x00, 0xf1, 0x98);
static const ble_uuid128_t data_service_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x01, 0x00, 0xf2, 0x98);
static const ble_uuid128_t data_rx_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x02, 0x00, 0xf2, 0x98);
static const ble_uuid128_t data_tx_uuid = BLE_UUID128_INIT(
    0x10, 0x10, 0xc9, 0x89, 0xf5, 0x6b, 0x50, 0x9e, 0xb1, 0x4c, 0x7c, 0x8f, 0x03, 0x00, 0xf2, 0x98);
static std::atomic<int> gateways[2]{{-1}, {-1}};
static std::atomic<bool> runtime_allowed{true}, candidates_allowed{true};
static uint16_t data_tx_handle;
static int management_conn = -1;
static uint8_t own_type;
static char device_name[24] = "Waveshare-YA";
const char *ble_name() { return device_name; }
static std::atomic<bool> synced{false};
static std::atomic<int> phone{-1};
static uint16_t tx_handle;
static std::atomic<int64_t> pair_until{0};
static int64_t rx_at = 0;
static char rx[1025]{}, reply[2048]{"{\"v\":1}\n"};
static size_t rx_len = 0;
static ble_addr_t addresses[16]{};
static int connecting_index = -1;
static std::atomic<bool> accepting_scan{false};
static ble_addr_t bond_addresses[6]{};
static SemaphoreHandle_t ble_mutex;
static int gap(struct ble_gap_event *, void *);
static void advertise();
static void note(const char *text) {
  Guard g(state_mutex);
  strlcpy(state.ble_note, text, sizeof(state.ble_note));
  publish(BLE);
}
static bool known(const ble_addr_t *a) {
  ble_addr_t peers[6];
  int n = 0;
  if (ble_store_util_bonded_peers(peers, &n, 6))
    return false;
  for (int i = 0; i < n; i++)
    if (!ble_addr_cmp(a, &peers[i]))
      return true;
  return false;
}
static void send_reply(uint16_t conn, const char *str) {
  strlcpy(reply, str, sizeof(reply));
  strlcat(reply, "\n", sizeof(reply));
  size_t n = strlen(reply), mtu = ble_att_mtu(conn);
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  for (size_t i = 0; i < n; i += chunk) {
    auto *om = ble_hs_mbuf_from_flat(reply + i, std::min(chunk, n - i));
    if (om)
      ble_gatts_notify_custom(conn, tx_handle, om);
  }
}
static void rpc(uint16_t conn, const char *text) {
  auto *j = flat_json_object(text, strlen(text)) ? cJSON_Parse(text) : nullptr;
  auto *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "v", 1);
  bool ok = false;
  const char *reason = "invalid_request";
  if (j) {
    auto *id = cJSON_GetObjectItem(j, "id");
    if (cJSON_IsNumber(id))
      cJSON_AddNumberToObject(result, "id", id->valuedouble);
    auto *v = cJSON_GetObjectItem(j, "v");
    auto *op = cJSON_GetObjectItem(j, "op");
    if (cJSON_IsNumber(v) && v->valueint == 1 && cJSON_IsString(op)) {
      if (!strcmp(op->valuestring, "status")) {
        auto s = snapshot();
        auto c = config();
        cJSON_AddStringToObject(result, "data_identity", data_identity().c_str());
        cJSON_AddStringToObject(result, "version", esp_app_get_description()->version);
        cJSON_AddStringToObject(result, "ip", s.ip);
        cJSON_AddNumberToObject(result, "brightness", c.brightness);
        cJSON_AddNumberToObject(result, "volume", c.volume);
        cJSON_AddBoolToObject(result, "mute", c.mute);
        cJSON_AddNumberToObject(result, "fps", c.fps);
        auto prefs = system_prefs();
        cJSON_AddNumberToObject(result, "sleep", effective_sleep(s.battery));
        cJSON_AddNumberToObject(result, "sleep_battery", prefs.sleep_battery);
        cJSON_AddNumberToObject(result, "sleep_usb", prefs.sleep_usb);
        cJSON_AddStringToObject(result, "wifi_ssid", s.wifi_ssid);
        cJSON_AddNumberToObject(result, "wifi_rssi", s.wifi_rssi);
        cJSON_AddBoolToObject(result, "time_valid", s.time_valid);
        cJSON_AddBoolToObject(result, "rtc_valid", s.rtc_valid);
        cJSON_AddNumberToObject(result, "last_time_sync", s.last_time_sync);
        cJSON_AddStringToObject(result, "ota_phase", ota_phase_name(s.ota_phase));
        cJSON_AddNumberToObject(result, "ota_progress", s.ota_progress);
        cJSON_AddNumberToObject(result, "ota_received", s.ota_received);
        cJSON_AddNumberToObject(result, "ota_total", s.ota_total);
        cJSON_AddBoolToObject(result, "wifi_connected", s.wifi_connected);
        cJSON_AddBoolToObject(result, "phone_connected", s.phone_connected);
        cJSON_AddBoolToObject(result, "ble_services_valid", s.ble_services_valid);
        cJSON_AddNumberToObject(result, "battery", s.battery.valid ? s.battery.percent : -1);
        cJSON_AddBoolToObject(result, "charging", s.battery.valid && s.battery.charging);
        add_battery_stats(result, s.battery.stats);
        ok = true;
      } else if (!strcmp(op->valuestring, "set")) {
        auto *k = cJSON_GetObjectItem(j, "key");
        auto *value = cJSON_GetObjectItem(j, "value");
        ok = cJSON_IsString(k) && cJSON_IsNumber(value) &&
             setting(k->valuestring, value->valueint) == ESP_OK;
        reason = cJSON_IsString(k) && !strcmp(k->valuestring, "sleep")
                     ? "use_sleep_battery_or_sleep_usb"
                     : "invalid_setting_or_busy";
      } else if (!strcmp(op->valuestring, "data_config")) {
        auto *value = cJSON_GetObjectItem(j, "json");
        ok = cJSON_IsString(value) && data_configure(value->valuestring);
        reason = "invalid_config_or_busy";
      } else if (!strcmp(op->valuestring, "data_status")) {
        cJSON_AddStringToObject(result, "data", data_diagnostics().c_str());
        ok = true;
      } else if (!strcmp(op->valuestring, "wifi")) {
        auto *ssid = cJSON_GetObjectItem(j, "ssid");
        auto *pass = cJSON_GetObjectItem(j, "password");
        ok = !ota_busy && cJSON_IsString(ssid) && cJSON_IsString(pass) &&
             strlen(ssid->valuestring) > 0 && strlen(ssid->valuestring) <= 32 &&
             strlen(pass->valuestring) <= 63 &&
             command(Op::WIFI_CONNECT, ssid->valuestring, pass->valuestring);
        reason = "invalid_wifi_or_busy";
      }
    }
  }
  cJSON_AddBoolToObject(result, "ok", ok);
  if (!ok)
    cJSON_AddStringToObject(result, "error", reason);
  auto *out = cJSON_PrintUnformatted(result);
  if (out) {
    send_reply(conn, out);
    free(out);
  }
  cJSON_Delete(result);
  cJSON_Delete(j);
}
static int access(uint16_t conn, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
  ble_gap_conn_desc d{};
  if ((conn != gateways[0] && conn != gateways[1]) || ble_gap_conn_find(conn, &d) ||
      !d.sec_state.encrypted || !d.sec_state.authenticated)
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  Guard lock(ble_mutex);
  if (management_conn != conn) {
    management_conn = conn;
    rx_len = 0;
  }
  if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR)
    return os_mbuf_append(ctx->om, reply, strlen(reply)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
  if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  if (now_ms() - rx_at > 5000)
    rx_len = 0;
  rx_at = now_ms();
  size_t n = OS_MBUF_PKTLEN(ctx->om);
  if (rx_len + n > 1024) {
    rx_len = 0;
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  }
  if (os_mbuf_copydata(ctx->om, 0, n, rx + rx_len))
    return BLE_ATT_ERR_UNLIKELY;
  rx_len += n;
  rx[rx_len] = 0;
  if (rx_len && rx[rx_len - 1] == '\n') {
    rpc(conn, rx);
    memset(rx, 0, sizeof(rx));
    rx_len = 0;
  }
  return 0;
}
static ble_gatt_chr_def chars[3]{};
static ble_gatt_chr_def data_chars[3]{};
static ble_gatt_svc_def services[3]{};
static int data_access(uint16_t conn, uint16_t, ble_gatt_access_ctxt *ctx, void *) {
  ble_gap_conn_desc d{};
  if ((conn != gateways[0] && conn != gateways[1]) || ble_gap_conn_find(conn, &d) ||
      !d.sec_state.encrypted || !d.sec_state.authenticated)
    return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
  if (ctx->op != BLE_GATT_ACCESS_OP_WRITE_CHR)
    return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
  uint8_t bytes[512];
  auto n = OS_MBUF_PKTLEN(ctx->om);
  if (n > sizeof bytes || os_mbuf_copydata(ctx->om, 0, n, bytes))
    return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
  return data_receive(conn, bytes, n) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}
static void advertise() {
  if (!synced || !config().ble || !runtime_allowed ||
      ((gateways[0] >= 0 && gateways[1] >= 0) ||
       (!candidates_allowed && (gateways[0] >= 0 || gateways[1] >= 0))) ||
      ble_gap_adv_active())
    return;
  ble_hs_adv_fields f{};
  f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  f.name = (uint8_t *)device_name;
  f.name_len = strlen(device_name);
  f.name_is_complete = 1;
  ble_gap_adv_set_fields(&f);
  ble_hs_adv_fields scan{};
  scan.uuids128 = (ble_uuid128_t *)&data_service_uuid;
  scan.num_uuids128 = 1;
  scan.uuids128_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&scan);
  ble_gap_adv_params p{};
  p.conn_mode = BLE_GAP_CONN_MODE_UND;
  p.disc_mode = BLE_GAP_DISC_MODE_GEN;
  ble_gap_adv_start(own_type, nullptr, BLE_HS_FOREVER, &p, gap, nullptr);
}
static void gatt_event(int kind, uint16_t conn, uint16_t handle, int status,
                       const ble_uuid_t *uuid = nullptr, os_mbuf *bytes = nullptr,
                       uint16_t start = 0, uint16_t end = 0) {
  GattEvent e{};
  e.kind = kind;
  e.connection = conn;
  e.handle = handle;
  e.status = status;
  e.start = start;
  e.end = end;
  if (uuid)
    ble_uuid_to_str(uuid, e.uuid);
  if (bytes) {
    e.length = std::min<unsigned>(OS_MBUF_PKTLEN(bytes), sizeof(e.data));
    os_mbuf_copydata(bytes, 0, e.length, e.data);
  }
  esp_event_post(EVENTS, GATT, &e, sizeof(e), 0);
}
static int data_cb(uint16_t conn, const ble_gatt_error *e, ble_gatt_attr *a, void *arg) {
  gatt_event((int)(intptr_t)arg, conn, a ? a->handle : 0, e->status, nullptr, a ? a->om : nullptr);
  return 0;
}
struct Discovery {
  int conn = -1;
  ble_gatt_svc services[16]{};
  ble_gatt_chr chars[32]{};
  int count = 0, si = 0, nchars = 0, ci = 0;
};
static Discovery discoveries[2];
static void next_service(Discovery *);
static int descriptor_cb(uint16_t conn, const ble_gatt_error *e, uint16_t chr_handle,
                         const ble_gatt_dsc *d, void *arg) {
  auto *x = (Discovery *)arg;
  if (!e->status && d)
    gatt_event(5, conn, d->handle, 0, &d->uuid.u, nullptr, chr_handle, 0);
  else if (e->status == BLE_HS_EDONE) {
    x->ci++;
    while (x->ci < x->nchars) {
      uint16_t end = x->ci + 1 < x->nchars ? x->chars[x->ci + 1].def_handle - 1
                                           : x->services[x->si].end_handle;
      uint16_t begin = x->chars[x->ci].val_handle;
      if (end > begin && ble_gattc_disc_all_dscs(conn, begin, end, descriptor_cb, x) == 0)
        return 0;
      x->ci++;
    }
    x->si++;
    next_service(x);
  } else if (e->status) {
    gatt_event(5, conn, 0, e->status);
    x->conn = -1;
  }
  return 0;
}
static int chr_cb(uint16_t conn, const ble_gatt_error *e, const ble_gatt_chr *c, void *arg) {
  auto *x = (Discovery *)arg;
  if (!e->status && c) {
    gatt_event(4, conn, c->val_handle, 0, &c->uuid.u, nullptr, c->def_handle, c->properties);
    if (x->nchars < 32)
      x->chars[x->nchars++] = *c;
    else
      gatt_event(4, conn, 0, BLE_HS_ENOMEM);
  } else if (e->status == BLE_HS_EDONE) {
    x->ci = -1;
    ble_gatt_error done{};
    done.status = BLE_HS_EDONE;
    descriptor_cb(conn, &done, 0, nullptr, x);
  } else if (e->status) {
    gatt_event(4, conn, 0, e->status);
    x->conn = -1;
  }
  return 0;
}
static void next_service(Discovery *x) {
  if (x->si >= x->count) {
    gatt_event(3, x->conn, 0, BLE_HS_EDONE);
    x->conn = -1;
    return;
  }
  x->nchars = 0;
  auto &s = x->services[x->si];
  int rc = ble_gattc_disc_all_chrs(x->conn, s.start_handle, s.end_handle, chr_cb, x);
  if (rc) {
    gatt_event(4, x->conn, 0, rc);
    x->conn = -1;
  }
}
static int svc_cb(uint16_t conn, const ble_gatt_error *e, const ble_gatt_svc *s, void *arg) {
  auto *x = (Discovery *)arg;
  if (!e->status && s) {
    gatt_event(3, conn, s->start_handle, 0, &s->uuid.u, nullptr, s->start_handle, s->end_handle);
    if (x->count < 16)
      x->services[x->count++] = *s;
    else
      gatt_event(3, conn, 0, BLE_HS_ENOMEM);
  } else if (e->status == BLE_HS_EDONE)
    next_service(x);
  else if (e->status) {
    gatt_event(3, conn, 0, e->status);
    x->conn = -1;
  }
  return 0;
}
static void discover(uint16_t conn) {
  for (auto &d : discoveries)
    if (d.conn < 0) {
      d = Discovery{};
      d.conn = conn;
      int rc = ble_gattc_disc_all_svcs(conn, svc_cb, &d);
      if (rc) {
        gatt_event(3, conn, 0, rc);
        d.conn = -1;
      }
      return;
    }
}
static int gap(ble_gap_event *e, void *context) {
  if (e->type == BLE_GAP_EVENT_DISC) {
    if (!accepting_scan)
      return 0;
    // The connection picker excludes non-connectable beacons so they cannot fill its bounded list.
    if (e->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND &&
        e->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_DIR_IND &&
        e->disc.event_type != BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
      return 0;
    ble_hs_adv_fields f{};
    ble_hs_adv_parse_fields(&f, e->disc.data, e->disc.length_data);
    Guard g(state_mutex);
    int i = 0;
    for (; i < state.peer_count; i++)
      if (!ble_addr_cmp(&addresses[i], &e->disc.addr))
        break;
    if (i == state.peer_count && e->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP)
      return 0;
    if (i >= 16) {
      int weakest = -1;
      for (int n = 0; n < 16; n++)
        if (!state.peers[n].connected &&
            (weakest < 0 || state.peers[n].rssi < state.peers[weakest].rssi))
          weakest = n;
      if (weakest < 0 || e->disc.rssi <= state.peers[weakest].rssi)
        return 0;
      i = weakest;
      state.peers[i] = Peer{};
      state.ble_scan_version++;
    }
    if (i == state.peer_count)
      state.peer_count++;
    addresses[i] = e->disc.addr;
    auto &p = state.peers[i];
    snprintf(p.address, sizeof(p.address), "%02x:%02x:%02x:%02x:%02x:%02x", addresses[i].val[5],
             addresses[i].val[4], addresses[i].val[3], addresses[i].val[2], addresses[i].val[1],
             addresses[i].val[0]);
    if (f.name && f.name_len) {
      size_t n = std::min((size_t)f.name_len, sizeof(p.name) - 1);
      if (strlen(p.name) != n || memcmp(p.name, f.name, n))
        state.ble_scan_version++;
      memcpy(p.name, f.name, n);
      p.name[n] = 0;
    } else if (!p.name[0])
      strlcpy(p.name, "BLE 设备", sizeof(p.name));
    p.rssi = e->disc.rssi;
  } else if (e->type == BLE_GAP_EVENT_DISC_COMPLETE) {
    accepting_scan = false;
  } else if (e->type == BLE_GAP_EVENT_CONNECT) {
    if (e->connect.status) {
      if (context) {
        Guard g(state_mutex);
        connecting_index = -1;
      }
      gatt_event(7, 0xffff, 0, e->connect.status);
      char message[64];
      snprintf(message, sizeof(message), "蓝牙连接失败 (%d)", e->connect.status);
      note(message);
      advertise();
      return 0;
    }
    ble_gap_conn_desc d{};
    ble_gap_conn_find(e->connect.conn_handle, &d);
    if (!config().ble || !runtime_allowed) {
      if (context) {
        Guard g(state_mutex);
        connecting_index = -1;
      }
      ble_gap_terminate(d.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
      return 0;
    }
    gatt_event(7, d.conn_handle, 0, 0);
    if (!context) {
      ESP_LOGI("ya_ble", "gateway admission known=%d window=%d", known(&d.peer_id_addr), now_ms() <= pair_until);
      if ((gateways[0] >= 0 && gateways[1] >= 0) ||
          (!candidates_allowed && (gateways[0] >= 0 || gateways[1] >= 0)) ||
          (!known(&d.peer_id_addr) && now_ms() > pair_until)) {
        ble_gap_terminate(d.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
      }
      gateways[gateways[0] < 0 ? 0 : 1] = d.conn_handle;
      if (phone < 0)
        phone = d.conn_handle;
      {
        Guard g(state_mutex);
        for (auto &link : state.gateways) if (link.handle < 0) {
          link = {}; link.handle = d.conn_handle;
          link.encrypted=d.sec_state.encrypted; link.authenticated=d.sec_state.authenticated;
          const auto *a = d.peer_id_addr.val;
          snprintf(link.address, sizeof(link.address), "%02x:%02x:%02x:%02x:%02x:%02x", a[5],a[4],a[3],a[2],a[1],a[0]);
          break;
        }
        state.phone_connected = true;
      }
      data_link(d.conn_handle, true);
      advertise();
      // Let the central finish GATT discovery before its authenticated read
      // triggers pairing. Starting SMP here can race Chrome's service discovery.
      // RX/TX still require encrypted + authenticated access at the ATT layer.
      note("手机或电脑已连接，等待安全配对");
    } else {
      {
        Guard g(state_mutex);
        for (int i = 0; i < state.peer_count; i++)
          if (i == connecting_index || !ble_addr_cmp(&addresses[i], &d.peer_ota_addr) ||
              !ble_addr_cmp(&addresses[i], &d.peer_id_addr)) {
            state.peers[i].connected = true;
            state.peers[i].handle = d.conn_handle;
          }
        connecting_index = -1;
      }
      discover(d.conn_handle);
      note("外部 BLE 设备已连接");
    }
  } else if (e->type == BLE_GAP_EVENT_DISCONNECT) {
    gatt_event(7, e->disconnect.conn.conn_handle, 0, e->disconnect.reason);
    for (auto &discovery : discoveries)
      if (discovery.conn == e->disconnect.conn.conn_handle)
        discovery.conn = -1;
    data_link(e->disconnect.conn.conn_handle, false);
    for (auto &g : gateways)
      if (g == e->disconnect.conn.conn_handle)
        g = -1;
    if (phone == e->disconnect.conn.conn_handle) {
      phone = gateways[0] >= 0 ? gateways[0].load() : gateways[1].load();
      rx_len = 0;
    }
    {
      Guard g(state_mutex);
      for (auto &p : state.peers)
        if (p.handle == e->disconnect.conn.conn_handle) {
          p.connected = false;
          p.handle = -1;
        }
    }
    advertise();
    {
      Guard g(state_mutex);
      for (auto &link : state.gateways) if (link.handle == e->disconnect.conn.conn_handle) link = {};
      state.phone_connected = phone >= 0;
      state.ble_disconnect_reason = e->disconnect.reason;
      if (phone < 0) state.pair_code_valid = false;
    }
    ESP_LOGI("ya_ble", "disconnect conn=%u reason=%d", e->disconnect.conn.conn_handle, e->disconnect.reason);
    note(phone >= 0 ? "部分设备已断开，手机或电脑仍连接" : "蓝牙连接已断开");
    notifications_add("蓝牙设备已断开", NoticeKind::BLE);
  } else if (e->type == BLE_GAP_EVENT_SUBSCRIBE) {
    // CCCD subscription proves service discovery has completed. Start SMP here,
    // before protected management operations, without racing GAP connection setup.
    if (e->subscribe.cur_notify && e->subscribe.attr_handle == tx_handle) {
      ble_gap_conn_desc desc{};
      if (!ble_gap_conn_find(e->subscribe.conn_handle,&desc) &&
          !(desc.sec_state.encrypted && desc.sec_state.authenticated)) {
        int rc=ble_gap_security_initiate(e->subscribe.conn_handle);
        ESP_LOGI("ya_ble","security after subscribe conn=%u rc=%d",e->subscribe.conn_handle,rc);
      }
    }
    if (e->subscribe.attr_handle == data_tx_handle) {
      { Guard g(state_mutex); for (auto &link : state.gateways)
          if (link.handle == e->subscribe.conn_handle) {
            link.subscribed = e->subscribe.cur_notify;
            if (!link.subscribed) link.ready = false;
          }
      }
      data_subscribed(e->subscribe.conn_handle, e->subscribe.cur_notify);
    }
  } else if (e->type == BLE_GAP_EVENT_ADV_COMPLETE)
    advertise();
  else if (e->type == BLE_GAP_EVENT_PASSKEY_ACTION) {
    ESP_LOGI("ya_ble","passkey action=%d window=%d",e->passkey.params.action,now_ms()<=pair_until);
    if (now_ms() > pair_until) {
      ble_gap_terminate(e->passkey.conn_handle, BLE_ERR_AUTH_FAIL);
      return 0;
    }
    if (e->passkey.params.action == BLE_SM_IOACT_DISP) {
      ble_sm_io io{};
      io.action = BLE_SM_IOACT_DISP;
      io.passkey = esp_random() % 1000000;
      {
        Guard g(state_mutex);
        state.pair_code = io.passkey;
        state.pair_code_valid = true;
      }
      ble_sm_inject_io(e->passkey.conn_handle, &io);
      note("请在手机或电脑输入屏幕上的配对码");
    }
  } else if (e->type == BLE_GAP_EVENT_ENC_CHANGE) {
    ble_gap_conn_desc d{};
    bool secure = !e->enc_change.status && !ble_gap_conn_find(e->enc_change.conn_handle, &d) &&
                  d.sec_state.encrypted && d.sec_state.authenticated;
    { Guard g(state_mutex); for (auto &link : state.gateways)
        if (link.handle == e->enc_change.conn_handle) {
          link.encrypted = d.sec_state.encrypted; link.authenticated = d.sec_state.authenticated;
        }
      state.pair_code_valid = false;
    }
    ESP_LOGI("ya_ble", "security conn=%u status=%d authenticated=%d", e->enc_change.conn_handle, e->enc_change.status, secure);
    note(secure ? "蓝牙安全连接已建立" : "蓝牙配对失败");
    notifications_add(secure ? "蓝牙安全连接已建立" : "蓝牙配对失败", NoticeKind::BLE);
    if (!secure) ble_gap_terminate(e->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
  }
  else if (e->type == BLE_GAP_EVENT_REPEAT_PAIRING) {
    if (now_ms() <= pair_until) {
      ble_gap_conn_desc d;
      ble_gap_conn_find(e->repeat_pairing.conn_handle, &d);
      ble_store_util_delete_peer(&d.peer_id_addr);
      return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    return BLE_GAP_REPEAT_PAIRING_IGNORE;
  } else if (e->type == BLE_GAP_EVENT_NOTIFY_RX) {
    gatt_event(6, e->notify_rx.conn_handle, e->notify_rx.attr_handle, 0, nullptr, e->notify_rx.om);
    char text[128];
    snprintf(text, sizeof(text), "GATT 通知 conn=%u attr=%u bytes=%u", e->notify_rx.conn_handle,
             e->notify_rx.attr_handle, (unsigned)OS_MBUF_PKTLEN(e->notify_rx.om));
    note(text);
  }
  publish(BLE);
  return 0;
}
static void sync() {
  ble_hs_id_infer_auto(0, &own_type);
  // NimBLE restores bonded IRKs before sync_cb. Enable controller resolution
  // before advertising so the unchanged bonded-peer gate sees the identity.
  int privacy_rc = ble_hs_pvcy_set_resolve_enabled(1);
  ESP_LOGI("ya_ble", "bonded address resolution status=%d", privacy_rc);
  synced = true;
  {
    Guard g(state_mutex);
    state.ble_ready = true;
    uint16_t mg = 0, data = 0;
    state.ble_services_valid = !ble_gatts_find_svc(&service_uuid.u, &mg) &&
                               !ble_gatts_find_svc(&data_service_uuid.u, &data);
    ESP_LOGI("ya_ble", "GATT management=98f10001 handle=%u data=98f20001 handle=%u valid=%d", mg, data, state.ble_services_valid);
  }
  ble_svc_gatt_changed(1, 0xffff);
  advertise();
}
static void host(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}
void ble_init() {
  ESP_LOGI("ble_budget", "internal=%u retention=%u largest_retention=%u",
           unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_RETENTION)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_RETENTION)));
  ble_mutex = xSemaphoreCreateMutex();
  auto e = nimble_port_init();
  if (e != ESP_OK) {
    note("蓝牙初始化失败");
    abort();
  }
  ble_svc_gap_init();
  ble_svc_gatt_init();
  uint8_t mac[6]{};
  ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
  snprintf(device_name,sizeof(device_name),"Waveshare-YA-%02X%02X",mac[4],mac[5]);
  ble_svc_gap_device_name_set(device_name);
  chars[0].uuid = &rx_uuid.u;
  chars[0].access_cb = access;
  chars[0].flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
  chars[1].uuid = &tx_uuid.u;
  chars[1].access_cb = access;
  chars[1].flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ_ENC |
                   BLE_GATT_CHR_F_READ_AUTHEN;
  chars[1].val_handle = &tx_handle;
  services[0].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[0].uuid = &service_uuid.u;
  services[0].characteristics = chars;
  data_chars[0].uuid = &data_rx_uuid.u;
  data_chars[0].access_cb = data_access;
  data_chars[0].flags =
      BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN;
  data_chars[1].uuid = &data_tx_uuid.u;
  data_chars[1].access_cb = data_access;
  data_chars[1].flags = BLE_GATT_CHR_F_NOTIFY;
  data_chars[1].val_handle = &data_tx_handle;
  services[1].type = BLE_GATT_SVC_TYPE_PRIMARY;
  services[1].uuid = &data_service_uuid.u;
  services[1].characteristics = data_chars;
  if (ble_gatts_count_cfg(services) || ble_gatts_add_svcs(services))
    abort();
  ble_hs_cfg.sync_cb = sync;
  ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.store_status_cb = [](struct ble_store_status_event *, void *) -> int {
    return BLE_HS_ESTORE_CAP;
  };
  ble_store_config_init();
  nimble_port_freertos_init(host);
}
void ble_apply() {
  if (!synced)
    return;
  if (!config().ble || !runtime_allowed) {
    accepting_scan = false;
    if (ble_gap_conn_active())
      ble_gap_conn_cancel();
    ble_gap_adv_stop();
    ble_gap_disc_cancel();
    for (auto &c : gateways)
      if (c >= 0)
        ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
    auto s = snapshot();
    for (auto &p : s.peers)
      if (p.connected)
        ble_gap_terminate(p.handle, BLE_ERR_REM_USER_CONN_TERM);
    note("蓝牙已关闭");
  } else
    advertise();
}
void ble_scan() {
  if (!synced || !config().ble) {
    notice("请先打开蓝牙");
    return;
  }
  if (ble_gap_conn_active()) {
    notice("正在连接，请稍后扫描");
    return;
  }
  accepting_scan = false;
  ble_gap_disc_cancel();
  {
    Guard g(state_mutex);
    int kept = 0;
    for (int i = 0; i < state.peer_count; i++) {
      if (!state.peers[i].connected)
        continue;
      state.peers[kept] = state.peers[i];
      addresses[kept] = addresses[i];
      kept++;
    }
    for (int i = kept; i < 16; i++) {
      state.peers[i] = Peer{};
      addresses[i] = {};
    }
    state.peer_count = kept;
    state.ble_scan_version++;
  }
  ble_gap_disc_params p{};
  p.passive = 0;
  p.itvl = 160;
  p.window = 48;
  p.filter_duplicates = 1;
  accepting_scan = true;
  if (ble_gap_disc(own_type, 10000, &p, gap, nullptr))
    accepting_scan = false;
  note("扫描蓝牙设备，持续十秒");
}
void ble_connect(int i, const char *expected_address) {
  accepting_scan = false;
  ble_gap_disc_cancel();
  auto s = snapshot();
  if (*expected_address) {
    i = -1;
    for (int n = 0; n < s.peer_count; n++)
      if (!strcmp(s.peers[n].address, expected_address))
        i = n;
  }
  if (!config().ble || i < 0 || i >= s.peer_count || s.peers[i].connected)
    return;
  if (ble_gap_conn_active()) {
    notice("正在连接，请稍候");
    return;
  }
  int n = 0;
  for (auto &p : s.peers)
    if (p.connected)
      n++;
  if (n >= 2) {
    note("最多连接两个外部设备");
    return;
  }
  ble_addr_t a;
  {
    Guard g(state_mutex);
    a = addresses[i];
    connecting_index = i;
  }
  ble_gap_disc_cancel();
  if (ble_gap_connect(own_type, &a, 15000, nullptr, gap, (void *)1)) {
    {
      Guard g(state_mutex);
      connecting_index = -1;
    }
    note("无法启动连接");
  }
}
void ble_disconnect(int i, const char *expected_address) {
  auto s = snapshot();
  if (*expected_address) {
    i = -1;
    for (int n = 0; n < s.peer_count; n++)
      if (!strcmp(s.peers[n].address, expected_address))
        i = n;
  }
  if (i >= 0 && i < s.peer_count && s.peers[i].connected)
    ble_gap_terminate(s.peers[i].handle, BLE_ERR_REM_USER_CONN_TERM);
}
void ble_forget(int i, const char *expected_address) {
  if (!synced)
    return;
  if (i < 0) {
    ble_addr_t peers[6];
    int count = 0;
    ble_store_util_bonded_peers(peers, &count, 6);
    for (int n = 0; n < count; n++)
      ble_gap_unpair(&peers[n]);
    note("已清除全部蓝牙配对");
    return;
  }
  ble_addr_t addr{};
  {
    Guard g(state_mutex);
    if (*expected_address) {
      i = -1;
      for (int n=0;n<state.bond_count;++n) if (!strcmp(state.bonds[n],expected_address)) i=100+n;
      if (i < 0) return;
    }
    if (i >= 100) {
      if (i - 100 >= state.bond_count)
        return;
      addr = bond_addresses[i - 100];
    } else {
      if (i >= state.peer_count)
        return;
      addr = addresses[i];
    }
  }
  ble_gap_unpair(&addr);
  note("已忘记蓝牙设备");
}
void ble_pair() {
  if (!config().ble || !runtime_allowed) {
    notice("请先打开蓝牙");
    return;
  }
  pair_until = now_ms() + 120000;
  {
    Guard g(state_mutex);
    state.pair_code = 0;
    state.pair_code_valid = false;
    state.pair_until = pair_until;
  }
  advertise();
  note("允许新设备配对，有效期两分钟");
}
void ble_tick() {
  if (!synced)
    return;
  // Restored bonds can already be encrypted before the UI link entry exists.
  // Refresh from the owning stack rather than relying only on ENC_CHANGE order.
  for (const auto &gateway : gateways) {
    int handle=gateway.load(); ble_gap_conn_desc desc{};
    if (handle<0 || ble_gap_conn_find(handle,&desc)) continue;
    Guard g(state_mutex);
    for (auto &link : state.gateways) if (link.handle==handle) {
      link.encrypted=desc.sec_state.encrypted;
      link.authenticated=desc.sec_state.authenticated;
    }
  }

  ble_addr_t bonds[6];
  int count = 0;
  ble_store_util_bonded_peers(bonds, &count, 6);
  {
    Guard g(state_mutex);
    state.bond_count = count;
    state.phone_connected = phone >= 0;
    state.ble_scanning = accepting_scan;
    state.pair_until = pair_until;
    for (int i = 0; i < count; i++) {
      bond_addresses[i] = bonds[i];
      const auto *v = bonds[i].val;
      snprintf(state.bonds[i], 32, "%02x:%02x:%02x:%02x:%02x:%02x", v[5], v[4], v[3], v[2], v[1],
               v[0]);
    }
  }
  if (pair_until && now_ms() > pair_until) {
    pair_until = 0;
    Guard g(state_mutex);
    state.pair_code = 0;
    state.pair_code_valid = false;
    state.pair_until = 0;
  }
}
void ble_data_ready(int conn, bool ready) {
  Guard g(state_mutex);
  for (auto &link : state.gateways) if (link.handle == conn) link.ready = ready;
}
esp_err_t ble_read(int c, uint16_t a) {
  return ble_gattc_read(c, a, data_cb, (void *)1) == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t ble_write(int c, uint16_t a, const void *b, size_t n) {
  if (n > 512)
    return ESP_ERR_INVALID_SIZE;
  return ble_gattc_write_flat(c, a, b, n, data_cb, (void *)2) == 0 ? ESP_OK : ESP_FAIL;
}
esp_err_t ble_subscribe(int c, uint16_t cccd) {
  uint16_t enable = 1;
  return ble_write(c, cccd, &enable, sizeof(enable));
}
bool ble_data_send(int c, const uint8_t *p, size_t n) {
  ble_gap_conn_desc d{};
  if (ble_gap_conn_find(c, &d) || !d.sec_state.encrypted || !d.sec_state.authenticated)
    return false;
  auto *m = ble_hs_mbuf_from_flat(p, n);
  return m && ble_gatts_notify_custom(c, data_tx_handle, m) == 0;
}
int ble_data_rssi(int c) {
  int8_t r = -127;
  return ble_gap_conn_rssi(c, &r) == 0 ? r : -127;
}
void ble_allow_candidates(bool allowed) {
  if (candidates_allowed.exchange(allowed) != allowed) {
    if (!allowed)
      ble_gap_adv_stop();
    else
      advertise();
  }
}
void ble_data_drop(int c, const char *reason) {
  { Guard g(state_mutex); strlcpy(state.ble_drop_note,reason,sizeof(state.ble_drop_note)); }
  ESP_LOGW("ya_ble","data disconnect conn=%d reason=%s",c,reason);
  ble_gap_terminate(c, BLE_ERR_REM_USER_CONN_TERM);
}
void ble_runtime(bool allowed) {
  if (runtime_allowed.exchange(allowed) != allowed)
    ble_apply();
  static bool was_idle = false;
  bool idle = screen_sleeping.load();
  if (allowed && idle != was_idle) {
    was_idle = idle;
    for (auto &c : gateways)
      if (c >= 0) {
        ble_gap_upd_params p{};
        p.itvl_min = idle ? 40 : 12;
        p.itvl_max = idle ? 80 : 24;
        p.latency = 0;
        p.supervision_timeout = 600;
        ble_gap_update_params(c, &p);
      }
  }
}
} // namespace ya
