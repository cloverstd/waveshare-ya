#include "data.hpp"
#include "companion.hpp"
#include "logs.hpp"
#include <memory>
#include "power.hpp"
#include "data_protocol.hpp"
#include "cJSON.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "host/ble_hs.h"
#include <deque>
#include <map>
#include <vector>
#include <algorithm>
#include <cmath>
#include <ctime>
namespace ya {
namespace {
std::string str(const cJSON *j, const char *k) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsString(v) ? v->valuestring : "";
}
int number(const cJSON *j, const char *k, int d = 0) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, k);
  return cJSON_IsNumber(v) ? v->valueint : d;
}
std::string dump(const cJSON *j) {
  char *s = cJSON_PrintUnformatted(j);
  std::string r = s ? s : "";
  free(s);
  return r;
}
bool tree(const cJSON *j) {
  if (cJSON_IsNumber(j) && !std::isfinite(j->valuedouble))
    return false;
  for (auto *a = j->child; a; a = a->next) {
    if (cJSON_IsObject(j)) {
      if (!a->string)
        return false;
      for (auto *b = a->next; b; b = b->next)
        if (b->string && !strcmp(a->string, b->string))
          return false;
    }
    if (!tree(a))
      return false;
  }
  return true;
}
cJSON *parse(const std::string &s) {
  if (!protocol::bounded_json(s) || s.find("\\u0000") != std::string::npos)
    return nullptr;
  const char *end = nullptr;
  auto *j = cJSON_ParseWithLengthOpts(s.c_str(), s.size() + 1, &end, true);
  if (!j || !cJSON_IsObject(j) || !tree(j)) {
    cJSON_Delete(j);
    return nullptr;
  }
  return j;
}
std::string canonical(const cJSON *j) {
  if (!j)
    return "{}";
  if (cJSON_IsObject(j)) {
    std::vector<const cJSON *> v;
    for (auto *c = j->child; c; c = c->next)
      v.push_back(c);
    std::sort(v.begin(), v.end(),
              [](auto *a, auto *b) { return strcmp(a->string, b->string) < 0; });
    std::string s = "{";
    for (auto *c : v) {
      auto *k = cJSON_CreateString(c->string);
      if (s.size() > 1)
        s += ",";
      s += dump(k) + ":" + canonical(c);
      cJSON_Delete(k);
    }
    return s + "}";
  }
  if (cJSON_IsArray(j)) {
    std::string s = "[";
    for (auto *c = j->child; c; c = c->next) {
      if (s.size() > 1)
        s += ",";
      s += canonical(c);
    }
    return s + "]";
  }
  return dump(j);
}
std::string uuid() {
  uint32_t r[4];
  esp_fill_random(r, sizeof r);
  char b[37];
  snprintf(b, sizeof b, "%08lx-%04x-%04x-%04x-%04x%08lx", (unsigned long)r[0], unsigned(r[1] >> 16),
           unsigned(r[1] & 65535), unsigned(r[2] >> 16), unsigned(r[2] & 65535),
           (unsigned long)r[3]);
  return b;
}
struct Message {
  int kind = 0, conn = -1;
  size_t len = 0;
  char text[4097]{};
};
QueueHandle_t queue;
SemaphoreHandle_t mutex;
std::atomic<bool> busy{false}, has_plans{false}, needs_wifi{false};
bool sync_requested = false;
struct Plan {
  std::string resource, params;
  unsigned seconds = 60;
  int64_t due = 0;
  bool background = false;
};
std::vector<Plan> plans;
struct Cache {
  DataView view;
  int64_t until = 0;
};
std::vector<Cache> cache;
std::vector<std::string> wake_resources;
std::string settings = "{}", service = "ya-local", scope = "default", endpoint, token;
std::string diag = "offline";
struct Tx {
  std::string body, bytes, id;
  size_t offset = 0;
  int tries = 0;
  int64_t at = 0;
  bool ack = false;
};
struct Peer {
  int conn = -1;
  protocol::Frame frame;
  std::string session;
  std::deque<Tx> tx;
  std::deque<std::string> seen;
  std::map<std::string, int> sequences;
  bool subscribed = false, ready = false, announced_active = false, companion = false;
  unsigned max_bytes = 4096;
  int64_t connected = 0, rx_at = 0, sample_at = 0;
  std::vector<int> samples;
  int rssi = -127, groups = 0;
  std::string probe;
  std::map<std::string, std::string> replies;
  std::shared_ptr<LogBuffer> log_batch;
  uint32_t log_cursor = 0, log_lost = 0;
  bool log_live = false;
  std::string log_finish;
  int64_t log_at = 0, log_deadline = 0;
};
Peer peers[2];
int active = -1;
int64_t switched = 0, probe_at = 0, candidate_until = 0;
struct Work {
  std::string request, resource, params, id, op, key;
  int conn = -1;
  int64_t deadline = 0;
  bool sent = false;
  int64_t sent_at = 0;
};
std::deque<Work> work;
uint32_t generation = 0;
int64_t disconnect_at = 0;
void config_status(const char *s) {
  Guard g(mutex);
  diag = s;
}
bool post(int kind, int conn, const void *p = nullptr, size_t n = 0) {
  if (!queue || n > 4096)
    return false;
  // Heap allocation avoids putting a 4 KiB command on the NimBLE or caller stack.
  auto *m = new (std::nothrow) Message;
  if (!m)
    return false;
  m->kind = kind;
  m->conn = conn;
  m->len = n;
  if (n)
    memcpy(m->text, p, n);
  if (xQueueSend(queue, &m, 0) != pdTRUE) {
    delete m;
    return false;
  }
  return true;
}
Peer *peer(int conn) {
  for (auto &p : peers)
    if (p.conn == conn)
      return &p;
  return nullptr;
}
cJSON *envelope(const char *type, const std::string &session) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "proto", "ya.data");
  cJSON_AddNumberToObject(j, "v", 1);
  cJSON_AddStringToObject(j, "type", type);
  cJSON_AddStringToObject(j, "id", uuid().c_str());
  cJSON_AddStringToObject(j, "session", session.c_str());
  cJSON_AddStringToObject(j, "service", service.c_str());
  cJSON_AddStringToObject(j, "scope", scope.c_str());
  return j;
}
bool send(Peer &p, cJSON *j, bool ack = false) {
  auto body = dump(j);
  if (body.size() > p.max_bytes || p.tx.size() >= 8)
    return false;
  Tx t{body, protocol::frame(body), str(j, "id")};
  t.ack = ack;
  // An ACK may pass an awaiting frame, but must never interrupt a partially sent frame.
  if (ack && (p.tx.empty() || p.tx.front().offset == p.tx.front().bytes.size()))
    p.tx.push_front(std::move(t));
  else
    p.tx.push_back(std::move(t));
  return true;
}
void acknowledge(Peer &p, const std::string &id) {
  auto *j = envelope("ack", p.session);
  cJSON_AddStringToObject(j, "reply_to", id.c_str());
  send(p, j, true);
  cJSON_Delete(j);
}
void error(Peer &p, const std::string &id, const char *code) {
  auto *j = envelope("error", p.session);
  cJSON_AddStringToObject(j, "reply_to", id.c_str());
  auto *e = cJSON_AddObjectToObject(j, "error");
  cJSON_AddStringToObject(e, "code", code);
  send(p, j);
  cJSON_Delete(j);
}
std::string key(const std::string &r, const std::string &p) { return r + "\n" + p; }
void store(cJSON *j, const std::string &r, const std::string &params) {
  auto body = dump(j);
  if (body.size() > 4096)
    return;
  Guard g(mutex);
  Cache *c = nullptr;
  for (auto &v : cache)
    if (key(v.view.resource, v.view.params) == key(r, params))
      c = &v;
  if (!c) {
    if (cache.size() >= 8)
      cache.erase(cache.begin());
    cache.push_back({});
    c = &cache.back();
  }
  auto err = str(cJSON_GetObjectItem(j, "error"), "code");
  if (!err.empty() && !c->view.json.empty()) {
    c->view.error = err;
    c->until = 0;
    return;
  }
  c->view = {r, params, body, str(cJSON_GetObjectItem(j, "error"), "code"), false};
  auto *u = cJSON_GetObjectItem(j, "updated_at");
  int ttl = number(j, "ttl_s");
  time_t now = time(nullptr);
  bool fresh = cJSON_IsNumber(u) && now >= 1704067200 && u->valuedouble <= now + 30 &&
               u->valuedouble + ttl > now && ttl > 0;
  c->until = fresh ? now_ms() + std::min<double>(ttl, u->valuedouble + ttl - now) * 1000 : 0;
}
bool matching(cJSON *j, const Work &w) {
  auto *p = cJSON_GetObjectItem(j, "params");
  return str(j, "resource") == w.resource && number(j, "schema") == 1 && canonical(p) == w.params;
}
std::shared_ptr<LogBuffer> log_snapshot(uint32_t &lost) {
  auto *memory = heap_caps_malloc(sizeof(LogBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!memory) return {};
  auto view = std::shared_ptr<LogBuffer>(new (memory) LogBuffer{}, [](LogBuffer *p) { p->~LogBuffer(); heap_caps_free(p); });
  return logs_snapshot(*view, lost) ? view : nullptr;
}
void log_page(cJSON *j, const LogBuffer &v, uint32_t lost, uint32_t &cursor, unsigned max_bytes) {
  cJSON_AddNumberToObject(j, "boot", logs_boot_id());
  cJSON_AddNumberToObject(j, "overwritten", v.overwritten);
  cJSON_AddNumberToObject(j, "dropped", lost);
  cJSON_AddNumberToObject(j, "missed", v.count && cursor && v.at(0).sequence > cursor + 1 ? v.at(0).sequence - cursor - 1 : 0);
  auto *a = cJSON_AddArrayToObject(j, "entries");
  unsigned remaining = 0;
  for (unsigned i = 0; i < v.count; ++i) {
    auto &e = v.at(i); if (e.sequence <= cursor) continue;
    auto *item = cJSON_CreateObject();
    cJSON_AddNumberToObject(item, "sequence", e.sequence); cJSON_AddNumberToObject(item, "level", e.level);
    cJSON_AddStringToObject(item, "text", e.text); cJSON_AddItemToArray(a, item);
    if (dump(j).size() + 512 > max_bytes || cJSON_GetArraySize(a) > 4) {
      cJSON_DeleteItemFromArray(a, cJSON_GetArraySize(a) - 1); remaining = 1; break;
    }
    cursor = e.sequence;
  }
  cJSON_AddNumberToObject(j, "next", cursor); cJSON_AddBoolToObject(j, "more", remaining);
}
void local_request(Peer &p, cJSON *j) {
  auto resource = str(j, "resource"), id = str(j, "id"), op = str(j, "op");
  cJSON *body = nullptr;
  if (resource == "info.text") body = companion_request(p.conn, j);
  else {
    body = cJSON_CreateObject(); cJSON_AddStringToObject(body, "status", "succeeded");
    if (op == "unsubscribe") { p.log_live = false; p.log_batch.reset(); p.log_finish.clear(); }
    else if (op == "get" || op == "subscribe") {
      if (p.max_bytes < 2048 || companion_busy()) { cJSON_Delete(body); error(p, id, "busy"); return; }
      uint32_t lost = 0; auto view = log_snapshot(lost);
      if (!view) { cJSON_Delete(body); error(p, id, "busy"); return; }
      uint32_t cursor = number(j, "boot", -1) == int(logs_boot_id()) ? std::max(0, number(j, "after")) : 0;
      auto *d = cJSON_AddObjectToObject(body, "data"); log_page(d, *view, lost, cursor, p.max_bytes);
      if (op == "subscribe") { p.log_live = true; p.log_cursor = cursor; }
    } else { cJSON_Delete(body); error(p, id, "unsupported"); return; }
  }
  auto *r = envelope("response", p.session);
  cJSON_AddStringToObject(r, "reply_to", id.c_str()); cJSON_AddStringToObject(r, "resource", resource.c_str());
  cJSON_AddNumberToObject(r, "schema", 1); cJSON_AddObjectToObject(r, "params");
  for (auto *c = body->child; c; c = c->next) cJSON_AddItemToObject(r, c->string, cJSON_Duplicate(c, true));
  auto encoded = dump(r);
  if (send(p, r)) { p.replies[id] = encoded; if (p.replies.size() > 16) p.replies.erase(p.replies.begin()); }
  cJSON_Delete(r); cJSON_Delete(body);
}
void pump_logs(Peer &p, int64_t now) {
  if (p.log_batch && now > p.log_deadline) { p.log_batch.reset(); p.log_finish.clear(); notice("日志发送超时"); }
  if (p.log_batch) power_hold(2000);
  if (!p.log_finish.empty() || companion_busy() || !p.ready || p.tx.size() >= 2 || now < p.log_at) return;
  p.log_at = now + 1000;
  auto view = p.log_batch;
  uint32_t lost = p.log_lost;
  if (!view && p.log_live) view = log_snapshot(lost);
  if (!view || (!p.log_batch && (!view->count || view->at(view->count - 1).sequence <= p.log_cursor))) return;
  auto *event = envelope("event", p.session);
  cJSON_AddStringToObject(event, "resource", "system.logs"); cJSON_AddNumberToObject(event, "schema", 1);
  auto *d = cJSON_AddObjectToObject(event, "data"); auto cursor = p.log_cursor;
  log_page(d, *view, lost, cursor, p.max_bytes);
  if (send(p, event)) {
    p.log_cursor = cursor;
    if (p.log_batch) {
      power_hold(2000);
      if (!view->count || cursor >= view->at(view->count - 1).sequence) p.log_finish = str(event, "id");
    }
  }
  cJSON_Delete(event);
}
void accept(Peer &p, const std::string &text) {
  auto *j = parse(text);
  if (!j)
    return;
  auto type = str(j, "type"), id = str(j, "id"), session = str(j, "session");
  if (str(j, "proto") != "ya.data" || number(j, "v") != 1 || !protocol::uuid(id.c_str()) ||
      !protocol::uuid(session.c_str()) || str(j, "service") != service ||
      str(j, "scope") != scope) {
    cJSON_Delete(j);
    return;
  }
  if (type == "hello") {
    if (!p.session.empty() && p.session != session) {
      cJSON_Delete(j);
      return;
    }
    int max_bytes = number(j, "max_bytes", 4096);
    if (max_bytes < 256 || max_bytes > 4096) {
      cJSON_Delete(j);
      ble_data_drop(p.conn);
      return;
    }
    p.max_bytes = max_bytes;
    p.companion = str(j, "companion").find("logs-v1") != std::string::npos;
    if (p.session.empty()) p.connected = now_ms();
    p.session = session;
    acknowledge(p, id);
    auto *h = envelope("hello", session);
    cJSON_AddNumberToObject(h, "max_bytes", 4096);
    cJSON_AddBoolToObject(h, "commands", false);
    cJSON_AddStringToObject(h, "companion", "text-image-v1,logs-v1");
    send(p, h);
    cJSON_Delete(h);
    if (p.probe.empty()) {
      auto *q = envelope("request", session);
      p.probe = str(q, "id");
      cJSON_AddStringToObject(q, "resource", "system.health");
      cJSON_AddNumberToObject(q, "schema", 1);
      cJSON_AddStringToObject(q, "op", "get");
      cJSON_AddObjectToObject(q, "params");
      send(p, q);
      cJSON_Delete(q);
    }
    cJSON_Delete(j);
    return;
  }
  if (p.session.empty() || session != p.session) {
    cJSON_Delete(j);
    return;
  }
  if (type == "ack") {
    auto ref = str(j, "reply_to");
    for (auto it = p.tx.begin(); it != p.tx.end(); ++it)
      if (it->id == ref && it->offset == it->bytes.size()) {
        if (it->id == p.log_finish) { p.log_finish.clear(); p.log_batch.reset(); notice("日志已发送到数据站"); }
        p.tx.erase(it);
        break;
      }
    cJSON_Delete(j);
    return;
  }
  acknowledge(p, id);
  if (type == "request" && p.replies.count(id)) {
    auto *reply = parse(p.replies[id]); if (reply) { send(p, reply); cJSON_Delete(reply); }
    cJSON_Delete(j); return;
  }
  if (std::find(p.seen.begin(), p.seen.end(), id) != p.seen.end()) {
    cJSON_Delete(j);
    return;
  }
  p.seen.push_back(id);
  if (p.seen.size() > 16)
    p.seen.pop_front();
  if (type == "request" && number(j, "schema") == 1 &&
      (str(j, "resource") == "info.text" || str(j, "resource") == "system.logs")) {
    local_request(p, j); cJSON_Delete(j); return;
  }
  if (type == "response" && str(j, "reply_to") == p.probe &&
      str(j, "resource") == "system.health" && str(j, "status") == "succeeded")
    { if (!p.ready) p.connected = now_ms(); p.ready = true; ble_data_ready(p.conn, true); }
  else if ((type == "response" || type == "error") && !work.empty() && p.conn == active &&
           work.front().conn == p.conn && str(j, "reply_to") == work.front().id &&
           matching(j, work.front())) {
    store(j, work.front().resource, work.front().params);
    if (work.front().op == "command" || work.front().op == "command_status")
      app_config_set("sys_command", 1, "{}", 3);
    work.pop_front();
  } else if (type == "event" && p.conn == active) {
    // Unsolicited events are accepted only for an existing plan and matching schema.
    for (auto &v : plans)
      if (v.resource == str(j, "resource") &&
          v.params == canonical(cJSON_GetObjectItem(j, "params")) && number(j, "schema") == 1) {
        auto event_key = key(v.resource, v.params);
        int seq = number(j, "sequence");
        if (seq <= 0 || seq <= p.sequences[event_key])
          break;
        bool gap = p.sequences[event_key] && seq != p.sequences[event_key] + 1;
        p.sequences[event_key] = seq;
        if (gap) {
          v.due = 0;
          sync_requested = true;
          break;
        }
        store(j, v.resource, v.params);
        if (cJSON_IsTrue(cJSON_GetObjectItem(j, "notify")) &&
            std::find(wake_resources.begin(), wake_resources.end(), v.resource) !=
                wake_resources.end())
          power_notify();
        break;
      }
  } else if (type != "response")
    error(p, id, "unsupported");
  cJSON_Delete(j);
}
void pump(Peer &p) {
  if (!p.subscribed || p.tx.empty())
    return;
  // Find a queued ACK without interrupting the current frame.
  if (p.tx.front().offset == p.tx.front().bytes.size() && !p.tx.front().ack) {
    auto it = std::find_if(p.tx.begin() + 1, p.tx.end(), [](const Tx &t) { return t.ack; });
    if (it != p.tx.end()) {
      auto t = std::move(*it);
      p.tx.erase(it);
      p.tx.push_front(std::move(t));
    }
  }
  auto &t = p.tx.front();
  if (t.offset < t.bytes.size()) {
    auto n = std::min<size_t>(20, t.bytes.size() - t.offset);
    if (ble_data_send(p.conn, (const uint8_t *)t.bytes.data() + t.offset, n)) {
      t.offset += n;
      t.at = now_ms();
    }
  } else if (t.ack)
    p.tx.pop_front();
  else if (now_ms() - t.at > protocol::ACK_TIMEOUT_MS) {
    if (++t.tries > 2) {
      ble_data_drop(p.conn,"ack_timeout");
      p.tx.clear();
    } else {
      t.offset = 0;
      t.at = now_ms();
    }
  }
}
void enqueue(cJSON *j) {
  if (work.size() >= 8)
    return;
  auto r = str(j, "resource"), op = str(j, "op");
  auto params = canonical(cJSON_GetObjectItem(j, "params"));
  if (r.empty() || r.size() > 80 || params.size() > 512)
    return;
  if (op != "get" && op != "command" && op != "command_status" && op != "cancel")
    return;
  if (op == "command" && !protocol::uuid(str(j, "idempotency_key").c_str()))
    return;
  work.push_back({dump(j), r, params, "", op, str(j, "idempotency_key"), -1,
                  now_ms() + (screen_sleeping ? 55000 : protocol::REQUEST_TIMEOUT_MS), false});
}
void configure(const std::string &s, bool persist) {
  auto *j = parse(s);
  if (!j) {
    config_status("config_invalid_json");
    return;
  }
  auto sv = str(j, "service"), sc = str(j, "scope"), url = str(j, "endpoint"), tk = str(j, "token");
  auto *a = cJSON_GetObjectItem(j, "plans");
  if (sv.empty() || sv.size() > 64 || sc.empty() || sc.size() > 64 || url.size() > 384 ||
      tk.size() > 256 || (!url.empty() && url.rfind("https://", 0) != 0) || !cJSON_IsArray(a) ||
      cJSON_GetArraySize(a) > 4) {
    config_status("config_invalid");
    cJSON_Delete(j);
    return;
  }
  std::vector<Plan> next;
  bool valid = true;
  for (auto *v = a->child; v; v = v->next) {
    auto r = str(v, "resource");
    auto *p = cJSON_GetObjectItem(v, "params");
    int sec = number(v, "seconds", 60);
    if (r.empty() || r.size() > 80 || !cJSON_IsObject(p) || canonical(p).size() > 512 || sec < 5 ||
        sec > 86400) {
      valid = false;
      break;
    }
    next.push_back({r, canonical(p), (unsigned)sec, 0, true});
  }
  if (!valid || (persist && app_config_set("sys_data", 1, s.c_str(), s.size() + 1) != ESP_OK)) {
    config_status("config_save_failed");
    cJSON_Delete(j);
    return;
  }
  wake_resources.clear();
  auto *wr = cJSON_GetObjectItem(j, "wake_resources");
  if (cJSON_IsArray(wr))
    for (auto *v = wr->child; v && wake_resources.size() < 4; v = v->next)
      if (cJSON_IsString(v))
        wake_resources.push_back(v->valuestring);
  settings = s;
  { Guard g(mutex); service = sv; scope = sc; }
  endpoint = url;
  token = tk;
  plans = std::move(next);
  has_plans = !plans.empty();
  // A saved plan must get one immediate round even when the display is asleep.
  sync_requested = true;
  work.clear();
  if (persist)
    disconnect_at = now_ms() + 1500;
  active = -1;
  generation++;
  {
    Guard g(mutex);
    cache.clear();
  }

  {
    Guard g(mutex);
    diag = "configured";
  }
  cJSON_Delete(j);
}
int http_event(esp_http_client_event_t *e) {
  auto *s = (std::string *)e->user_data;
  if (e->event_id == HTTP_EVENT_ON_DATA) {
    if (s->size() + e->data_len > 4096)
      return ESP_FAIL;
    s->append((char *)e->data, e->data_len);
  }
  return ESP_OK;
}
void wifi_work(Work &w) {
  auto *j = parse(w.request);
  if (!j)
    return;
  auto session = uuid();
  cJSON_AddStringToObject(j, "session", session.c_str());
  cJSON_AddStringToObject(j, "service", service.c_str());
  cJSON_AddStringToObject(j, "scope", scope.c_str());
  cJSON_AddStringToObject(j, "id", uuid().c_str());
  cJSON_AddStringToObject(j, "proto", "ya.data");
  cJSON_AddNumberToObject(j, "v", 1);
  cJSON_AddStringToObject(j, "type", "request");
  w.id = str(j, "id");
  auto body = dump(j);
  cJSON_Delete(j);
  std::string result;
  esp_http_client_config_t c{};
  c.url = endpoint.c_str();
  c.method = HTTP_METHOD_POST;
  c.timeout_ms = 3000;
  c.crt_bundle_attach = esp_crt_bundle_attach;
  c.event_handler = http_event;
  c.user_data = &result;
  c.disable_auto_redirect = true;
  auto h = esp_http_client_init(&c);
  if (!h)
    return;
  esp_http_client_set_header(h, "Content-Type", "application/json");
  if (!token.empty())
    esp_http_client_set_header(h, "Authorization", ("Bearer " + token).c_str());
  esp_http_client_set_post_field(h, body.c_str(), body.size());
  auto e = esp_http_client_perform(h);
  int status = esp_http_client_get_status_code(h);
  esp_http_client_cleanup(h);
  auto *r = parse(result);
  if (e == ESP_OK && status == 200 && r && str(r, "proto") == "ya.data" && number(r, "v") == 1 &&
      str(r, "reply_to") == w.id && str(r, "session") == session && str(r, "service") == service &&
      str(r, "scope") == scope && matching(r, w))
    store(r, w.resource, w.params);
  else {
    Guard g(mutex);
    diag = "https_failed";
  }
  cJSON_Delete(r);
}
void process(Message &m) {
  if (m.kind == 1) {
    for (auto &p : peers)
      if (p.conn < 0) {
        p = Peer{};
        p.conn = m.conn;
        p.connected = now_ms();
        return;
      }
    ble_data_drop(m.conn);
  } else if (m.kind == 2) {
    companion_disconnect(m.conn);
    auto *p = peer(m.conn);
    if (p)
      *p = Peer{};
    if (active == m.conn) {
      active = -1;
      generation++;
      if (!work.empty() && work.front().sent) {
        auto &w = work.front();
        if (w.op == "get") {
          w.sent = false;
          w.conn = -1;
        } else {
          Guard g(mutex);
          diag = "unknown_result";
          work.pop_front();
        }
      }
    }
  } else if (m.kind == 3) {
    auto *p = peer(m.conn);
    if (!p)
      return;
    if (now_ms() - p->rx_at > 5000)
      p->frame.reset();
    p->rx_at = now_ms();
    std::string out;
    if (!p->frame.feed((uint8_t *)m.text, m.len, out)) {
      ble_data_drop(m.conn,"invalid_frame");
      return;
    }
    if (!out.empty())
      accept(*p, out);
  } else if (m.kind == 4) {
    auto *p = peer(m.conn);
    if (p)
      p->subscribed = m.len != 0;
  } else if (m.kind == 5)
    configure(m.text, true);
  else if (m.kind == 6) {
    auto *j = parse(m.text);
    if (j) {
      enqueue(j);
      cJSON_Delete(j);
    }
  } else if (m.kind == 7) {
    sync_requested = true;
    for (auto &p : plans)
      p.due = 0;
  } else if (m.kind == 11) {
    Peer *target = peer(active);
    if (!target || !target->ready || !target->companion) for (auto &p : peers) if (p.ready && p.companion) { target = &p; break; }
    if (!target || !target->ready || !target->companion || target->max_bytes < 2048) { notice("请先连接蓝牙数据站"); return; }
    if (companion_busy()) { notice("文字传输中，请稍后发送日志"); return; }
    target->log_batch = log_snapshot(target->log_lost); target->log_cursor = 0; target->log_finish.clear();
    target->log_deadline = now_ms() + 120000;
    if (!target->log_batch) notice("日志忙，请重试");
    else power_hold(2000);
  } else if (m.kind == 10) {
    work.clear();
    busy = false;
    for (auto &p : peers) {
      p.tx.clear();
      p.frame.reset();
    }
  } else if (m.kind == 8 || m.kind == 9) {
    auto *j = parse(m.text);
    if (!j)
      return;
    auto r = str(j, "resource"), pa = canonical(cJSON_GetObjectItem(j, "params"));
    auto it = std::find_if(plans.begin(), plans.end(),
                           [&](const Plan &p) { return p.resource == r && p.params == pa; });
    if (m.kind == 9) {
      if (it != plans.end() && !it->background)
        plans.erase(it);
    } else if (it == plans.end() && plans.size() < 8)
      plans.push_back({r, pa, (unsigned)std::max(5, number(j, "seconds", 60)), 0, false});
    cJSON_Delete(j);
  }
}
void worker(void *) {
  for (;;) {
    Message *m = nullptr;
    if (xQueueReceive(queue, &m,
                      pdMS_TO_TICKS((peers[0].conn >= 0 || peers[1].conn >= 0) ? 10 : 500)) ==
        pdTRUE) {
      process(*m);
      delete m;
    }
    companion_tick();
    int64_t now = now_ms();
    if (disconnect_at) {
      if (now >= disconnect_at) {
        for (auto &p : peers)
          if (p.conn >= 0)
            ble_data_drop(p.conn);
        disconnect_at = 0;
      } else
        continue;
    }

    for (auto &p : peers)
      if (p.conn >= 0) {
        pump(p);
        pump_logs(p, now);
        if (!p.ready && protocol::handshake_expired(!p.session.empty(), now - p.connected)) {
          ble_data_drop(p.conn,"handshake_timeout");
          continue;
        }
        if (p.ready && now - p.sample_at >= 600) {
          p.sample_at = now;
          int r = ble_data_rssi(p.conn);
          if (r > -127) {
            p.samples.push_back(r);
            if (p.samples.size() == 5) {
              auto s = p.samples;
              std::sort(s.begin(), s.end());
              p.rssi = s[2];
              p.samples.clear();
              if (active < 0) {
                active = p.conn;
                p.announced_active = false;
                switched = now;
                generation++;
                {
                  Guard g(mutex);
                  diag = "ble active=" + std::to_string(active) + " rssi=" + std::to_string(p.rssi);
                }
              } else if (p.conn != active) {
                auto *a = peer(active);
                if (a && protocol::better(a->rssi, p.rssi, true, now - switched >= 60000))
                  p.groups++;
                else
                  p.groups = 0;
                if (p.groups >= 2) {
                  int old = active;
                  active = p.conn;
                  p.announced_active = false;
                  switched = now;
                  generation++;
                  if (!work.empty() && work.front().sent) {
                    if (work.front().op == "get") {
                      work.front().sent = false;
                      work.front().conn = -1;
                    } else {
                      Guard g(mutex);
                      diag = "unknown_result";
                      work.pop_front();
                    }
                  }
                  ble_data_drop(old,"better_gateway");
                }
              }
            }
          }
        }
        if (p.conn != active && p.ready && now - p.connected > 20000)
          ble_data_drop(p.conn,"inactive_gateway");
        if (p.conn == active && p.ready && !p.announced_active) {
          // A client cannot infer route selection from the transport hello.
          // Announce it even if sleeping plans are not currently due.
          auto *event = envelope("event", p.session);
          cJSON_AddStringToObject(event, "resource", "system.gateway");
          cJSON_AddNumberToObject(event, "schema", 1);
          cJSON_AddObjectToObject(event, "params");
          auto *data = cJSON_AddObjectToObject(event, "data");
          cJSON_AddBoolToObject(data, "active", true);
          p.announced_active = send(p, event);
          cJSON_Delete(event);
        }
      }
    bool sleeping = screen_sleeping.load();
    auto profile = power_profile();
    if (active < 0 || ((!sleeping || !work.empty()) && now - probe_at >= 300000)) {
      probe_at = now;
      candidate_until = now + 20000;
    }
    ble_allow_candidates(active < 0 || now < candidate_until);
    needs_wifi = profile.route != 1 && !endpoint.empty() && !work.empty() &&
                 (profile.route == 2 || active < 0);

    for (auto &p : plans)
      if ((!sleeping || (sync_requested && p.background)) && now >= p.due && work.size() < 8) {
        auto *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "resource", p.resource.c_str());
        cJSON_AddStringToObject(j, "op", "get");
        cJSON_AddNumberToObject(j, "schema", 1);
        cJSON_AddItemToObject(j, "params", cJSON_Parse(p.params.c_str()));
        enqueue(j);
        cJSON_Delete(j);
        p.due = now +
                int64_t(sleeping ? std::max<unsigned>(60, profile.sync_minutes * 60) : p.seconds) *
                    1000;
      }
    sync_requested = false;
    busy = !work.empty();
    if (work.empty())
      continue;
    auto &w = work.front();
    if (now >= w.deadline) {
      Guard g(mutex);
      diag = w.op == "get" ? "timeout" : "unknown_result";
      work.pop_front();
      continue;
    }
    if (w.sent) {
      if (now - w.sent_at > protocol::RESPONSE_TIMEOUT_MS) {
        ble_data_drop(w.conn,"resource_timeout");
        if (w.op == "get") {
          w.sent = false;
          w.conn = -1;
          active = -1;
        } else {
          Guard g(mutex);
          diag = "unknown_result";
          work.pop_front();
        }
      }
      continue;
    }
    if (w.op == "command" &&
        app_config_set("sys_command", 1, w.request.c_str(), w.request.size() + 1) != ESP_OK) {
      Guard g(mutex);
      diag = "command_save_failed";
      work.pop_front();
      continue;
    }
    auto *a = peer(active);
    if (a && a->ready && profile.route != 2) {
      auto *j = parse(w.request);
      if (!j) {
        work.pop_front();
        continue;
      }
      auto *h = envelope("request", a->session);
      for (auto *c = j->child; c; c = c->next)
        cJSON_AddItemToObject(h, c->string, cJSON_Duplicate(c, true));
      w.id = str(h, "id");
      if (send(*a, h)) {
        w.conn = a->conn;
        w.sent = true;
        w.sent_at = now;
      }
      cJSON_Delete(h);
      cJSON_Delete(j);
    } else if (profile.route != 1 && !endpoint.empty() &&
               (profile.route == 2 || now > w.deadline - (sleeping ? 45000 : 5000))) {
      bool online;
      {
        Guard g(state_mutex);
        online = state.wifi_connected;
      }
      if (online) {
        wifi_work(w);
        work.pop_front();
      }
    }
  }
}
} // namespace
void data_init() {
  companion_init();
  mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(20, sizeof(Message *));
  if (!mutex || !queue)
    abort();
  auto *b = new char[4096]{};
  size_t n = 4096;
  if (app_config_get("sys_data", 1, b, &n) == ESP_OK && n && b[n - 1] == 0)
    configure(b, false);
  delete[] b;
  // Cached responses remain stale until refreshed after any reboot.
  auto *c = new char[4096]{};
  n = 4096;
  if (app_config_get("sys_cache", 1, c, &n) == ESP_OK && n && c[n - 1] == 0) {
    auto *j = parse(c);
    auto *a = cJSON_GetObjectItem(j, "entries");
    if (cJSON_IsArray(a))
      for (auto *v = a->child; v; v = v->next) {
        auto r = str(v, "resource"), p = canonical(cJSON_GetObjectItem(v, "params"));
        store(v, r, p);
      }
    cJSON_Delete(j);
    for (auto &v : cache)
      v.until = 0;
  }
  delete[] c;
  auto *pending = new char[4096]{};
  n = 4096;
  if (app_config_get("sys_command", 1, pending, &n) == ESP_OK && n && pending[n - 1] == 0) {
    auto *j = parse(pending);
    if (j && !str(j, "resource").empty()) {
      auto *op = cJSON_GetObjectItem(j, "op");
      if (op)
        cJSON_SetValuestring(op, "command_status");
      enqueue(j);
    }
    cJSON_Delete(j);
  }
  delete[] pending;
  static StackType_t stack[8192];
  static StaticTask_t task;
  if (!xTaskCreateStatic(worker, "data", sizeof stack, nullptr, 4, stack, &task))
    abort();
}
bool companion_send_logs() { return post(11, -1); }
std::string data_identity() { Guard g(mutex); auto *j = cJSON_CreateObject(); cJSON_AddStringToObject(j, "service", service.c_str()); cJSON_AddStringToObject(j, "scope", scope.c_str()); auto s = dump(j); cJSON_Delete(j); return s; }
void data_link(int c, bool v) { post(v ? 1 : 2, c); }
bool data_receive(int c, const uint8_t *p, size_t n) { return post(3, c, p, n); }
void data_subscribed(int c, bool v) {
  uint8_t b = 1;
  post(4, c, v ? &b : nullptr, v ? 1 : 0);
}
bool data_configure(const char *s) { return s && post(5, -1, s, strlen(s)); }
bool data_request(const char *r, const char *p, const char *op, const char *body, const char *k) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "resource", r);
  cJSON_AddStringToObject(j, "op", op);
  cJSON_AddNumberToObject(j, "schema", 1);
  auto *params = parse(p);
  auto *d = parse(body);
  if (!params || !d) {
    cJSON_Delete(params);
    cJSON_Delete(d);
    cJSON_Delete(j);
    return false;
  }
  cJSON_AddItemToObject(j, "params", params);
  cJSON_AddItemToObject(j, "data", d);
  if (*k)
    cJSON_AddStringToObject(j, "idempotency_key", k);
  auto s = dump(j);
  cJSON_Delete(j);
  return post(6, -1, s.data(), s.size());
}
bool data_subscribe(const char *r, const char *p, unsigned seconds) {
  auto *j = parse(p);
  if (!j)
    return false;
  auto *q = cJSON_CreateObject();
  cJSON_AddStringToObject(q, "resource", r);
  cJSON_AddNumberToObject(q, "seconds", seconds);
  cJSON_AddItemToObject(q, "params", j);
  auto s = dump(q);
  cJSON_Delete(q);
  return post(8, -1, s.data(), s.size());
}
void data_unsubscribe(const char *r, const char *p) {
  auto *j = parse(p);
  if (!j)
    return;
  auto *q = cJSON_CreateObject();
  cJSON_AddStringToObject(q, "resource", r);
  cJSON_AddItemToObject(q, "params", j);
  auto s = dump(q);
  cJSON_Delete(q);
  post(9, -1, s.data(), s.size());
}
DataView data_view(const char *r, const char *p) {
  auto *j = parse(p);
  auto pa = canonical(j);
  cJSON_Delete(j);
  Guard g(mutex);
  for (auto &v : cache)
    if (v.view.resource == r && v.view.params == pa) {
      auto out = v.view;
      out.fresh = v.until > now_ms();
      return out;
    }
  return {r, pa, "", "missing", false};
}
std::string data_summary() {
  Guard g(mutex);
  std::string s;
  for (auto &v : cache) {
    auto *j = parse(v.view.json);
    auto *d = cJSON_GetObjectItem(j, "data");
    bool weather = v.view.resource == "weather.current", quote = v.view.resource == "market.quote";
    s += weather ? "天气预报" : quote ? "股票行情" : "其他信息";
    s += v.until > now_ms() ? " · 已更新\n" : " · 数据过期\n";
    if (!v.view.error.empty())
      s += "暂不可用：" + v.view.error + "\n";
    else if (weather) {
      s += "温度 " + dump(cJSON_GetObjectItem(d, "temperature_c")) + " °C\n湿度 " +
           dump(cJSON_GetObjectItem(d, "humidity_pct")) + "%\n";
      auto *a = cJSON_GetObjectItem(d, "forecast");
      if (cJSON_IsArray(a))
        for (auto *f = a->child; f; f = f->next) {
          time_t t = number(f, "time");
          tm date{};
          gmtime_r(&t, &date);
          char day[20];
          strftime(day, sizeof day, "%m-%d", &date);
          s += std::string(day) + "  " + dump(cJSON_GetObjectItem(f, "min_c")) + " / " +
               dump(cJSON_GetObjectItem(f, "max_c")) + " °C\n";
        }
    } else if (quote) {
      auto *p = cJSON_GetObjectItem(j, "params");
      s += str(p, "market") + " " + str(p, "symbol") + "\n" + str(d, "price") + " " +
           str(d, "currency") + "\n涨跌幅 " + str(d, "change_pct") + "%\n";
    } else
      s += str(d, "text") + "\n";
    auto source = str(d, "source");
    if (!source.empty())
      s += "来源 " + source + "\n";
    auto *u = cJSON_GetObjectItem(j, "updated_at");
    if (cJSON_IsNumber(u)) {
      time_t t = u->valuedouble;
      tm local{};
      localtime_r(&t, &local);
      char at[40];
      strftime(at, sizeof at, "%m-%d %H:%M", &local);
      s += std::string("更新于 ") + at + "\n";
    }
    s += "\n";
    cJSON_Delete(j);
    if (s.size() > 1800)
      break;
  }
  return s.empty() ? "等待数据，先在网页配置后台资源" : s;
}
std::string data_diagnostics() {
  Guard g(mutex);
  return diag;
}
bool data_busy() { return busy.load(); }
bool data_has_plans() { return has_plans.load(); }
bool data_needs_wifi() { return needs_wifi.load(); }
void data_sync() { post(7, -1); }
void data_cancel_round() { post(10, -1); }
bool data_checkpoint() {
  Guard g(mutex);
  auto *j = cJSON_CreateObject();
  auto *a = cJSON_AddArrayToObject(j, "entries");
  for (auto &v : cache) {
    auto *c = parse(v.view.json);
    if (c) {
      cJSON_AddItemToArray(a, c);
      if (dump(j).size() + 1 > 4096)
        cJSON_DeleteItemFromArray(a, cJSON_GetArraySize(a) - 1);
    }
  }
  auto s = dump(j);
  cJSON_Delete(j);
  if (s.size() + 1 > 4096)
    return false;
  return app_config_set("sys_cache", 1, s.c_str(), s.size() + 1) == ESP_OK;
}
} // namespace ya
