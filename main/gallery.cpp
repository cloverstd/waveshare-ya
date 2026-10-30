#ifdef YA_GALLERY_HOST_TEST
#include "gallery_host.hpp"
#else
#include "gallery.hpp"
#include "power.hpp"
#include "esp_spiffs.h"
#include "nvs.h"
#include "esp_ota_ops.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#endif
#include <map>
#include <cctype>
#include <set>
#include <deque>
#include <dirent.h>
#include <unistd.h>
#ifndef YA_GALLERY_FS_ROOT
#define YA_GALLERY_FS_ROOT "/spiffs"
#endif
namespace ya::gallery {
namespace {
using Bytes = std::vector<uint8_t>;
SemaphoreHandle_t mutex;
QueueHandle_t queue;
std::shared_ptr<const Catalog> published;
std::shared_ptr<const View> rendered;
std::atomic<int> upload_connection{-1}, cancel_connection{-1};
std::map<std::string, int64_t> refreshed;
std::atomic<uint32_t> generation{0}, next_job{0};
std::atomic<bool> working{false}, receiving{false}, clear_cache{false};
Catalog catalog;
bool loaded = false, broken = false;
struct Job {
  uint32_t id;
  int conn;
  std::string json, result, error;
  std::atomic<bool> cancel{false};
  bool done = false;
};
std::deque<std::shared_ptr<Job>> jobs;
struct Work {
  std::shared_ptr<Job> job;
  std::string album;
  uint32_t gen = 0;
  int page = 0, index = -1;
  bool force = false;
};
struct Upload {
  int conn = -1;
  std::string id, album, photo, sha;
  size_t size = 0;
  int64_t start = 0, touched = 0;
  Bytes bytes;
} upload;
struct Cached {
  std::string key;
  std::shared_ptr<Image> image;
};
std::deque<Cached> cache;
std::string str(const cJSON *j, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
int number(const cJSON *j, const char *key, int fallback = -1) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= INT32_MAX &&
                 v->valuedouble == v->valueint
             ? v->valueint
             : fallback;
}
std::string dump(const cJSON *j) {
  char *b = cJSON_PrintUnformatted(j);
  std::string s = b ? b : "";
  free(b);
  return s;
}
std::string hash(const uint8_t *b, size_t n) {
  uint8_t digest[32];
  mbedtls_sha256(b, n, digest, 0);
  char out[65];
  for (unsigned i = 0; i < 32; ++i)
    snprintf(out + 2 * i, 3, "%02x", digest[i]);
  return out;
}
std::string random_id() {
  char b[17];
  snprintf(b, sizeof b, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  return b;
}
std::string path(const char *kind, const std::string &sha) {
  return std::string(YA_GALLERY_FS_ROOT) + "/yg" + kind + "_" + sha.substr(0, 24);
}
Album *album(Catalog &c, const std::string &id) {
  for (auto &a : c.albums)
    if (a.id == id)
      return &a;
  return nullptr;
}
cJSON *serialize(const Catalog &c) {
  auto *j = cJSON_CreateObject();
  cJSON_AddNumberToObject(j, "schema", 1);
  cJSON_AddNumberToObject(j, "revision", c.revision);
  auto *a = cJSON_AddArrayToObject(j, "albums");
  for (const auto &v : c.albums) {
    auto *x = cJSON_CreateObject();
    cJSON_AddItemToArray(a, x);
    cJSON_AddStringToObject(x, "id", v.id.c_str());
    cJSON_AddStringToObject(x, "name", v.name.c_str());
    cJSON_AddStringToObject(x, "source", v.source.c_str());
    cJSON_AddStringToObject(x, "manifest", v.manifest.c_str());
    auto *photos = cJSON_AddArrayToObject(x, "photos");
    for (const auto &p : v.photos) {
      auto *q = cJSON_CreateObject();
      cJSON_AddItemToArray(photos, q);
      cJSON_AddStringToObject(q, "id", p.id.c_str());
      cJSON_AddStringToObject(q, "url", p.url.c_str());
      cJSON_AddStringToObject(q, "thumbnail_url", p.thumbnail_url.c_str());
      cJSON_AddStringToObject(q, "file", p.file.c_str());
    }
  }
  return j;
}
bool parse_catalog(const Bytes &b, Catalog &c) {
  if (!bounded_document(std::string_view(reinterpret_cast<const char *>(b.data()), b.size())))
    return false;
  const char *end = nullptr;
  auto *j =
      cJSON_ParseWithLengthOpts(reinterpret_cast<const char *>(b.data()), b.size(), &end, false);
  if (!j)
    return false;
  for (auto *q = end; q < reinterpret_cast<const char *>(b.data() + b.size()); ++q)
    if (!std::isspace(static_cast<unsigned char>(*q))) {
      cJSON_Delete(j);
      return false;
    }
  auto *list = cJSON_GetObjectItemCaseSensitive(j, "albums");
  bool ok = number(j, "schema") == 1 && cJSON_IsArray(list) &&
            cJSON_GetArraySize(list) <= int(MAX_ALBUMS);
  c = {};
  c.revision = number(j, "revision", 0);
  if (ok)
    for (auto *a = list->child; a; a = a->next) {
      Album v;
      v.id = str(a, "id");
      v.name = str(a, "name");
      v.source = str(a, "source");
      v.manifest = str(a, "manifest");
      auto *photos = cJSON_GetObjectItemCaseSensitive(a, "photos");
      if (!cJSON_IsArray(photos) || cJSON_GetArraySize(photos) > 90) {
        ok = false;
        break;
      }
      for (auto *p = photos->child; p; p = p->next)
        v.photos.push_back({str(p, "id"), str(p, "url"), str(p, "thumbnail_url"), str(p, "file")});
      c.albums.push_back(std::move(v));
    }
  cJSON_Delete(j);
  return ok && valid(c);
}
bool mounted() {
  Guard g(state_mutex);
  return state.flash_mounted;
}
bool read_file(const std::string &p, Bytes &bytes, size_t max) {
  FILE *f = fopen(p.c_str(), "rb");
  if (!f)
    return false;
  bool ok = fseek(f, 0, SEEK_END) == 0;
  long n = ftell(f);
  ok = ok && n > 0 && size_t(n) <= max && fseek(f, 0, SEEK_SET) == 0;
  if (ok) {
    bytes.resize(n);
    ok = fread(bytes.data(), 1, n, f) == size_t(n);
  }
  fclose(f);
  return ok;
}
bool read_object(const char *kind, const std::string &sha, Bytes &b, size_t max) {
  return digest_valid(sha) && read_file(path(kind, sha), b, max) && hash(b.data(), b.size()) == sha;
}
bool write_object(const char *kind, const std::string &sha, const Bytes &b) {
  Bytes existing;
  auto p = path(kind, sha);
  if (read_file(p, existing, MAX_CATALOG))
    return hash(existing.data(), existing.size()) == sha;
  size_t total = 0, used = 0;
  if (esp_spiffs_info("storage", &total, &used) != ESP_OK || !room(total, used, b.size() + 4096))
    return false;
  FILE *f = fopen(p.c_str(), "wb");
  if (!f)
    return false;
  bool ok = true;
  for (size_t o = 0; ok && o < b.size(); o += 4096) {
    size_t n = std::min<size_t>(4096, b.size() - o);
    ok = fwrite(b.data() + o, 1, n, f) == n;
  }
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok)
    ok = read_object(kind, sha, existing, MAX_CATALOG);
  if (!ok)
    unlink(p.c_str());
  return ok;
}
// Roots from both OTA slots are retained. Unknown/corrupt roots disable GC.
void collect() {
  Guard m(mutation_mutex);
  if (ota_busy)
    return;
  std::set<std::string> keep;
  for (const char *slot : {"ota_0", "ota_1"}) {
    char root[65]{};
    size_t n = sizeof root;
    auto e = app_config_get_slot(slot, "gallery", 1, root, &n);
    if (e == ESP_ERR_NVS_NOT_FOUND)
      continue;
    if (e != ESP_OK || n != 65 || root[64])
      return;
    Bytes b;
    Catalog c;
    {
      Guard s(storage_mutex);
      if (!read_object("c", root, b, MAX_CATALOG) || !parse_catalog(b, c))
        return;
    }
    keep.insert(path("c", root));
    for (const auto &a : c.albums)
      for (const auto &p : a.photos)
        if (!p.file.empty())
          keep.insert(path("p", p.file));
  }
  Guard s(storage_mutex);
  DIR *dir = opendir(YA_GALLERY_FS_ROOT);
  if (!dir)
    return;
  std::vector<std::string> stale;
  while (auto *e = readdir(dir)) {
    std::string name = e->d_name;
    if ((name.rfind("ygc_", 0) == 0 || name.rfind("ygp_", 0) == 0) && name.size() == 28) {
      auto p = std::string(YA_GALLERY_FS_ROOT) + "/" + name;
      if (!keep.count(p))
        stale.push_back(p);
    }
  }
  closedir(dir);
  for (const auto &p : stale)
    unlink(p.c_str());
}
void publish_catalog() {
  Guard g(mutex);
  published = std::make_shared<Catalog>(catalog);
}
bool load() {
  if (loaded)
    return !broken;
  if (!mounted()) {
    Guard g(mutex);
    if (!published) {
      auto c = std::make_shared<Catalog>();
      c->error = "内置存储未挂载\n请在设置中检查存储";
      published = c;
    }
    return false;
  }
  char root[65]{};
  size_t n = sizeof root;
  auto e = app_config_get("gallery", 1, root, &n);
  if (e == ESP_ERR_NVS_NOT_FOUND) {
    catalog = {};
    loaded = true;
    publish_catalog();
    collect();
    return true;
  }
  Bytes b;
  {
    Guard s(storage_mutex);
    broken = e != ESP_OK || n != 65 || root[64] || !read_object("c", root, b, MAX_CATALOG) ||
             !parse_catalog(b, catalog);
  }
  loaded = true;
  if (broken) {
    Guard g(mutex);
    auto c = std::make_shared<Catalog>();
    c->error = "相册目录校验失败\n请保留数据并检查日志";
    published = c;
  }
  if (!broken) {
    publish_catalog();
    collect();
  }
  return !broken;
}
bool save(Catalog next) {
  if (!valid(next) || ota_busy)
    return false;
  next.revision = catalog.revision;
  auto *before = serialize(catalog), *after = serialize(next);
  bool same = dump(before) == dump(after);
  cJSON_Delete(before);
  cJSON_Delete(after);
  if (same)
    return true;
  next.revision = catalog.revision + 1;
  auto *j = serialize(next);
  auto json = dump(j);
  cJSON_Delete(j);
  if (json.empty() || json.size() > MAX_CATALOG)
    return false;
  Bytes b(json.begin(), json.end());
  std::string sha = hash(b.data(), b.size());
  {
    Guard m(mutation_mutex);
    if (ota_busy)
      return false;
    Guard s(storage_mutex);
    if (!mounted() || !write_object("c", sha, b))
      return false;
  }
  if (app_config_set("gallery", 1, sha.c_str(), 65) != ESP_OK)
    return false;
  catalog = std::move(next);
  publish_catalog();
  cache.clear();
  collect();
  return true;
}
bool stop(const std::shared_ptr<Job> &j, uint32_t gen) {
  return ota_busy || power_paused || (gen && screen_sleeping) || (j && j->cancel) ||
         (gen && generation != gen);
}
bool fetch(const std::string &url, Bytes &b, size_t limit, const std::shared_ptr<Job> &job = {},
           uint32_t gen = 0) {
  if (!url_valid(url))
    return false;
  {
    Guard g(state_mutex);
    if (!state.wifi_connected)
      return false;
  }
  std::string current = url;
  int64_t deadline = now_ms() + 30000;
  for (int redirect = 0; redirect <= 3; ++redirect) {
    if (stop(job, gen) || now_ms() > deadline)
      return false;
    std::string location;
    esp_http_client_config_t c{};
    c.user_data = &location;
    c.event_handler = [](esp_http_client_event_t *e) -> esp_err_t {
      if (e->event_id == HTTP_EVENT_ON_HEADER && e->header_key && e->header_value &&
          !strcasecmp(e->header_key, "Location"))
        *static_cast<std::string *>(e->user_data) = e->header_value;
      return ESP_OK;
    };
    c.url = current.c_str();
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.timeout_ms = 1000;
    c.disable_auto_redirect = true;
    c.buffer_size = 2048;
    auto client = esp_http_client_init(&c);
    if (!client)
      return false;
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
      auto length = esp_http_client_fetch_headers(client);
      int status = esp_http_client_get_status_code(client);
      if (status >= 300 && status <= 308 && status != 304) {
        std::string next = location;
        if (next.rfind("/", 0) == 0) {
          size_t slash = current.find('/', current.find("://") + 3);
          next = current.substr(0, slash) + next;
        }
        bool allowed = url_valid(next) &&
                       !(current.rfind("https://", 0) == 0 && next.rfind("http://", 0) == 0);
        esp_http_client_cleanup(client);
        if (!allowed)
          return false;
        current = next;
        continue;
      }
      if (status == 200 && (length < 0 || uint64_t(length) <= limit)) {
        b.clear();
        uint8_t block[2048];
        while (!stop(job, gen) && now_ms() < deadline) {
          int n = esp_http_client_read(client, reinterpret_cast<char *>(block), sizeof block);
          if (n < 0)
            break;
          if (n == 0) {
            ok = esp_http_client_is_complete_data_received(client);
            break;
          }
          if (b.size() + n > limit)
            break;
          b.insert(b.end(), block, block + n);
        }
      }
    }
    esp_http_client_cleanup(client);
    return ok && !b.empty() && !stop(job, gen);
  }
  return false;
}
void cancel_upload() {
  upload = {};
  receiving = false;
  upload_connection = -1;
}
bool add_local(Catalog &next, const std::string &aid, const std::string &pid, const Bytes &input,
               std::string &error) {
  auto *a = album(next, aid);
  if (!a || a->source != "local") {
    error = "invalid_album";
    return false;
  }
  auto found =
      std::find_if(a->photos.begin(), a->photos.end(), [&](auto &p) { return p.id == pid; });
  if (!pid.empty() && found == a->photos.end()) {
    error = "invalid_photo";
    return false;
  }
  if (found == a->photos.end() && a->photos.size() >= MAX_LOCAL) {
    error = "photo_limit";
    return false;
  }
  auto image = decode(input.data(), input.size(), 432, 384, error);
  if (!image)
    return false;
  auto bytes = encode(*image);
  if (bytes.empty()) {
    error = "encode_failed";
    return false;
  }
  auto sha = hash(bytes.data(), bytes.size());
  {
    Guard m(mutation_mutex);
    if (ota_busy) {
      error = "busy";
      return false;
    }
    Guard s(storage_mutex);
    if (!mounted() || !write_object("p", sha, bytes)) {
      error = "storage_full_or_error";
      return false;
    }
  }
  Photo p{pid.empty() ? random_id() : pid, "", "", sha};
  if (found == a->photos.end())
    a->photos.push_back(p);
  else
    *found = p;
  return true;
}
bool refresh(Catalog &next, Album &a, const std::shared_ptr<Job> &job = {}, uint32_t gen = 0) {
  Bytes b;
  if (!fetch(a.manifest, b, MAX_CATALOG, job, gen))
    return false;
  if (!bounded_document(std::string_view(reinterpret_cast<const char *>(b.data()), b.size())))
    return false;
  const char *end = nullptr;
  auto *j =
      cJSON_ParseWithLengthOpts(reinterpret_cast<const char *>(b.data()), b.size(), &end, false);
  if (!j)
    return false;
  for (auto *q = end; q < reinterpret_cast<const char *>(b.data() + b.size()); ++q)
    if (!std::isspace(static_cast<unsigned char>(*q))) {
      cJSON_Delete(j);
      return false;
    }
  auto *photos = cJSON_GetObjectItemCaseSensitive(j, "photos");
  bool ok = number(j, "schema") == 1 && cJSON_IsArray(photos) && cJSON_GetArraySize(photos) <= 90;
  if (ok) {
    a.photos.clear();
    for (auto *p = photos->child; p; p = p->next)
      a.photos.push_back({str(p, "id"), str(p, "url"), str(p, "thumbnail_url"), ""});
    ok = valid(next);
  }
  cJSON_Delete(j);
  return ok;
}
template <class T> bool move_item(std::vector<T> &v, const std::string &id, int to) {
  if (to < 0 || to >= int(v.size()))
    return false;
  auto p = std::find_if(v.begin(), v.end(), [&](auto &a) { return a.id == id; });
  if (p == v.end())
    return false;
  auto item = std::move(*p);
  v.erase(p);
  v.insert(v.begin() + to, std::move(item));
  return true;
}
cJSON *handle(const std::shared_ptr<Job> &job, std::string &error) {
  auto *j = cJSON_Parse(job->json.c_str());
  if (!j) {
    error = "invalid_request";
    return nullptr;
  }
  struct Cleanup {
    cJSON *j;
    ~Cleanup() { cJSON_Delete(j); }
  } cleanup{j};
  if (stop(job, 0)) {
    error = "cancelled";
    return nullptr;
  }
  if (!load()) {
    error = broken ? "catalog_corrupt" : "storage_unavailable";
    return nullptr;
  }
  auto resource = str(j, "resource"), op = str(j, "op"), aid = str(j, "album"),
       pid = str(j, "photo");
  auto *result = cJSON_CreateObject();
  auto fail = [&](const char *e) -> cJSON * {
    error = e;
    cJSON_Delete(result);
    return nullptr;
  };
  if (resource == "gallery.albums" && op == "list") {
    auto *a = cJSON_AddArrayToObject(result, "albums");
    for (const auto &v : catalog.albums) {
      auto *x = cJSON_CreateObject();
      cJSON_AddItemToArray(a, x);
      cJSON_AddStringToObject(x, "id", v.id.c_str());
      cJSON_AddStringToObject(x, "name", v.name.c_str());
      cJSON_AddStringToObject(x, "source", v.source.c_str());
      cJSON_AddStringToObject(x, "manifest", v.manifest.c_str());
      cJSON_AddNumberToObject(x, "count", v.photos.size());
    } // two albums per message
    int offset = number(j, "offset", 0);
    while (cJSON_GetArraySize(a) > offset + 2)
      cJSON_DeleteItemFromArray(a, cJSON_GetArraySize(a) - 1);
    for (int i = 0; i < offset && a->child; ++i)
      cJSON_DeleteItemFromArray(a, 0);
    cJSON_AddNumberToObject(result, "total", catalog.albums.size());
    return result;
  }
  auto next = catalog;
  auto *a = album(next, aid);
  if (resource == "gallery.photos" && op == "list") {
    if (!a)
      return fail("invalid_album");
    auto *list = cJSON_AddArrayToObject(result, "photos");
    int offset = number(j, "offset", 0);
    for (size_t i = offset; i < a->photos.size() && i < size_t(offset) + 2; ++i) {
      const auto &p = a->photos[i];
      auto *x = cJSON_CreateObject();
      cJSON_AddItemToArray(list, x);
      cJSON_AddStringToObject(x, "id", p.id.c_str());
      cJSON_AddStringToObject(x, "url", p.url.c_str());
      cJSON_AddStringToObject(x, "thumbnail_url", p.thumbnail_url.c_str());
    }
    cJSON_AddNumberToObject(result, "total", a->photos.size());
    return result;
  }
  if (resource == "gallery.sd" && op == "list") {
    std::string dir = str(j, "path");
    if (dir.empty())
      dir = "photos";
    if (dir != "photos" && !path_valid(dir))
      return fail("invalid_path");
    auto *list = cJSON_AddArrayToObject(result, "files");
    struct Context {
      cJSON *list;
    };
    Context ctx{list};
    auto e = storage_list(
        dir.c_str(), number(j, "offset", 0), 8,
        [](const char *name, void *ctx) {
          cJSON_AddItemToArray(static_cast<Context *>(ctx)->list, cJSON_CreateString(name));
        },
        &ctx);
    if (e != ESP_OK)
      return fail("sd_unavailable");
    return result;
  }
  if (resource == "gallery.transfer") {
    auto id = str(j, "transfer");
    if (op == "cancel") {
      if (upload.conn == job->conn && upload.id == id)
        cancel_upload();
      return result;
    }
    if (op == "begin") {
      if (receiving)
        return fail("busy");
      int n = number(j, "size");
      auto sha = str(j, "sha256");
      if (!a || a->source != "local" || !id_valid(id) || n <= 0 || n > int(MAX_JPEG) ||
          !digest_valid(sha) || (!pid.empty() && !id_valid(pid)))
        return fail("invalid_upload");
      upload = {};
      upload.conn = job->conn;
      upload.id = id;
      upload.album = aid;
      upload.photo = pid;
      upload.sha = sha;
      upload.size = n;
      upload.start = upload.touched = now_ms();
      upload.bytes.reserve(n);
      receiving = true;
      upload_connection = job->conn;
      power_hold(2000);
      return result;
    }
    if (upload.conn != job->conn || upload.id != id)
      return fail("unknown_transfer");
    upload.touched = now_ms();
    if (op == "chunk") {
      int offset = number(j, "offset");
      auto text = str(j, "bytes");
      uint8_t block[1536];
      size_t n = 0;
      if (offset < 0 || text.size() > 2048 ||
          mbedtls_base64_decode(block, sizeof block, &n,
                                reinterpret_cast<const uint8_t *>(text.data()), text.size()) ||
          !n || size_t(offset) > upload.bytes.size() || size_t(offset) + n > upload.size)
        return fail("invalid_chunk");
      if (size_t(offset) < upload.bytes.size()) {
        if (offset + n > upload.bytes.size() || memcmp(upload.bytes.data() + offset, block, n))
          return fail("offset_mismatch");
      } else
        upload.bytes.insert(upload.bytes.end(), block, block + n);
      cJSON_AddNumberToObject(result, "offset", upload.bytes.size());
      return result;
    }
    if (op != "commit")
      return fail("unsupported");
    if (upload.bytes.size() != upload.size)
      return fail("incomplete");
    if (hash(upload.bytes.data(), upload.bytes.size()) != upload.sha) {
      cancel_upload();
      return fail("checksum_mismatch");
    }
    bool ok = add_local(next, upload.album, upload.photo, upload.bytes, error);
    cancel_upload();
    if (!ok) {
      cJSON_Delete(result);
      return nullptr;
    }
  } else if (resource == "gallery.albums") {
    if (op == "create") {
      if (next.albums.size() >= MAX_ALBUMS)
        return fail("album_limit");
      Album v;
      v.id = random_id();
      v.name = str(j, "name");
      v.source = str(j, "source");
      v.manifest = str(j, "manifest");
      cJSON_AddStringToObject(result, "album", v.id.c_str());
      next.albums.push_back(std::move(v));
    } else if (!a)
      return fail("invalid_album");
    else if (op == "update") {
      a->name = str(j, "name");
      if (a->source == "manifest")
        a->manifest = str(j, "manifest");
    } else if (op == "delete")
      next.albums.erase(std::remove_if(next.albums.begin(), next.albums.end(),
                                       [&](auto &v) { return v.id == aid; }),
                        next.albums.end());
    else if (op == "move") {
      if (!move_item(next.albums, aid, number(j, "position")))
        return fail("invalid_position");
    } else if (op == "refresh") {
      if (a->source != "manifest" || !refresh(next, *a, job))
        return fail("manifest_failed");
    } else
      return fail("unsupported");
  } else if (resource == "gallery.photos") {
    if (!a || a->source == "manifest")
      return fail("read_only_album");
    if (op == "add" || op == "update") {
      if (a->source != "urls")
        return fail("invalid_album");
      if (pid.empty())
        pid = random_id();
      if (!id_valid(pid))
        return fail("invalid_photo");
      auto p =
          std::find_if(a->photos.begin(), a->photos.end(), [&](auto &v) { return v.id == pid; });
      Photo value{pid, str(j, "url"), str(j, "thumbnail_url"), ""};
      if (p == a->photos.end()) {
        if (op == "update")
          return fail("invalid_photo");
        a->photos.push_back(value);
      } else
        *p = value;
    } else if (op == "delete")
      a->photos.erase(
          std::remove_if(a->photos.begin(), a->photos.end(), [&](auto &v) { return v.id == pid; }),
          a->photos.end());
    else if (op == "move") {
      if (!move_item(a->photos, pid, number(j, "position")))
        return fail("invalid_position");
    } else
      return fail("unsupported");
  } else if (resource == "gallery.sd" && op == "import") {
    auto rel = str(j, "path");
    if (!path_valid(rel))
      return fail("invalid_path");
    Bytes bytes;
    std::vector<uint8_t> block(65536);
    for (size_t offset = 0; offset <= MAX_FILE; offset += 65536) {
      if (stop(job, 0))
        return fail("cancelled");
      size_t got = 0;
      if (storage_read(rel.c_str(), offset, block.data(), block.size(), &got) != ESP_OK)
        return fail("sd_read_failed");
      if (bytes.size() + got > MAX_FILE)
        return fail("image_too_large");
      bytes.insert(bytes.end(), block.begin(), block.begin() + got);
      if (got < block.size())
        break;
    }
    if (!add_local(next, aid, pid, bytes, error)) {
      cJSON_Delete(result);
      return nullptr;
    }
  } else
    return fail("unsupported");
  if (stop(job, 0))
    return fail("cancelled");
  if (!valid(next))
    return fail("invalid_or_limit");
  if (!save(std::move(next)))
    return fail("storage_full_or_error");
  return result;
}
std::shared_ptr<Image> image(const Photo &p, bool thumb, std::string &error, uint32_t gen) {
  auto source =
      p.file.empty() ? (thumb && !p.thumbnail_url.empty() ? p.thumbnail_url : p.url) : p.file;
  auto key = source + (thumb ? "/t" : "/f");
  for (auto i = cache.begin(); i != cache.end(); ++i)
    if (i->key == key) {
      auto result = i->image;
      auto entry = *i;
      cache.erase(i);
      cache.push_back(entry);
      return result;
    }
  Bytes b;
  bool ok;
  if (p.file.empty())
    ok = fetch(source, b, MAX_FILE, {}, gen);
  else {
    Guard s(storage_mutex);
    ok = read_object("p", p.file, b, MAX_JPEG);
  }
  if (!ok) {
    error = "加载失败，点击重试";
    return {};
  }
  auto result = decode(b.data(), b.size(), 432, thumb ? 384 : 340, error);
  if (!result) {
    error = "图片格式不支持";
    return {};
  }
  if (thumb) {
    result = thumbnail(*result);
    if (!result) {
      error = "内存不足";
      return {};
    }
  }
  // Decode at screen resolution; the UI crops/scales thumbnails without I/O.
  size_t used = result->size();
  for (const auto &v : cache)
    used += v.image->size();
  while (!cache.empty() && (used > 960 * 1024 || cache.size() >= 12)) {
    used -= cache.front().image->size();
    cache.pop_front();
  }
  cache.push_back({key, result});
  return result;
}
void render(const Work &w) {
  if (w.gen != generation || !load())
    return;
  if (w.index == -2) {
    auto output = std::make_shared<View>();
    output->generation = w.gen;
    output->revision = 1;
    output->images.resize(catalog.albums.size());
    output->errors.resize(catalog.albums.size());
    for (size_t i = 0; i < catalog.albums.size() && !stop({}, w.gen); ++i) {
      if (!catalog.albums[i].photos.empty())
        output->images[i] = image(catalog.albums[i].photos[0], true, output->errors[i], w.gen);
      ++output->revision;
      if (w.gen == generation) {
        Guard g(mutex);
        rendered = std::make_shared<View>(*output);
      }
    }
    return;
  }
  auto *a = album(catalog, w.album);
  if (!a)
    return;
  std::string note;
  if (a->source == "manifest" &&
      (w.force || !refreshed.count(a->id) || now_ms() - refreshed[a->id] > 60000)) {
    refreshed[a->id] = now_ms();
    auto next = catalog;
    auto *target = album(next, w.album);
    if (refresh(next, *target, {}, w.gen) && !stop({}, w.gen))
      save(std::move(next));
    else
      note = "更新失败，显示上次清单";
    a = album(catalog, w.album);
  }
  auto output = std::make_shared<View>();
  output->generation = w.gen;
  output->revision = 1;
  int count = w.index < 0 ? 9 : 1;
  output->images.resize(count);
  output->errors.resize(count);
  auto publish = [&] {
    if (w.gen == generation) {
      Guard g(mutex);
      rendered = std::make_shared<View>(*output);
    }
  };
  publish();
  for (int i = 0; i < count && !stop({}, w.gen); ++i) {
    int index = w.index < 0 ? w.page * 9 + i : wrap(w.index, a->photos.size());
    if (index >= int(a->photos.size()))
      break;
    output->images[i] = image(a->photos[index], w.index < 0, output->errors[i], w.gen);
    if (!note.empty() && output->errors[i].empty())
      output->errors[i] = note;
    ++output->revision;
    publish();
  }
  if (w.index >= 0 && a->photos.size() > 1)
    for (int step : {1, -1})
      if (!stop({}, w.gen))
        image(a->photos[wrap(w.index + step, a->photos.size())], false, note, w.gen);
}
void worker(void *) {
  for (;;) {
    if (clear_cache.exchange(false))
      cache.clear();
    Work *raw = nullptr;
    if (xQueueReceive(queue, &raw, pdMS_TO_TICKS(200)) == pdTRUE) {
      std::unique_ptr<Work> w(raw);
      working = true;
      if (w->job) {
        std::string error;
        auto *result = handle(w->job, error);
        std::string text = result ? dump(result) : "{}";
        cJSON_Delete(result);
        {
          Guard g(mutex);
          w->job->result = text;
          w->job->error = error;
          w->job->done = true;
        }
        if (!error.empty())
          collect();
      } else
        render(*w);
      working = false;
    }
    if (receiving) {
      if (cancel_connection.exchange(-1) == upload.conn || ota_busy ||
          now_ms() - upload.touched > 45000 || now_ms() - upload.start > 1200000)
        cancel_upload();
      else
        power_hold(2000);
    }
    if (!loaded)
      load();
    if (screen_sleeping) {
      ++generation;
      cache.clear();
      Guard g(mutex);
      rendered.reset();
    }
  }
}
cJSON *response(const char *error = nullptr) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "status", error ? "failed" : "succeeded");
  if (error)
    cJSON_AddStringToObject(cJSON_AddObjectToObject(j, "error"), "code", error);
  else
    cJSON_AddObjectToObject(j, "data");
  return j;
}
} // namespace
void init() {
  mutex = xSemaphoreCreateMutex();
  queue = xQueueCreate(8, sizeof(Work *));
  if (!mutex || !queue)
    abort();
  static StackType_t stack[8192];
  static StaticTask_t task;
  if (!xTaskCreateStatic(worker, "gallery", sizeof stack, nullptr, 3, stack, &task))
    abort();
}
bool busy() { return working || receiving || uxQueueMessagesWaiting(queue); }
std::shared_ptr<const Catalog> snapshot() {
  Guard g(mutex);
  return published;
}
std::shared_ptr<const View> view_snapshot() {
  Guard g(mutex);
  return rendered;
}
void cancel_view() {
  clear_cache = true;
  ++generation;
  Guard g(mutex);
  rendered.reset();
}
uint32_t view(const std::string &aid, int page, int index, bool refresh) {
  uint32_t gen = ++generation;
  auto *w = new Work;
  w->album = aid;
  w->page = std::max(0, page);
  w->index = index;
  w->gen = gen;
  w->force = refresh;
  if (xQueueSend(queue, &w, 0) != pdTRUE) {
    delete w;
    return 0;
  }
  return gen;
}
void disconnect(int conn) {
  Guard g(mutex);
  for (auto &j : jobs)
    if (j->conn == conn && !j->done)
      j->cancel = true;
  if (upload_connection == conn)
    cancel_connection = conn; // upload expires if no further chunks arrive
}
cJSON *request(int conn, const cJSON *j) {
  auto resource = str(j, "resource"), op = str(j, "op");
  Guard g(mutex);
  if (resource == "gallery.jobs") {
    int id = number(j, "job_id");
    for (auto &job : jobs)
      if (int(job->id) == id && job->conn == conn) {
        if (op == "cancel")
          job->cancel = true;
        else if (op != "get")
          return response("unsupported");
        auto *r = response();
        auto *d = cJSON_GetObjectItem(r, "data");
        cJSON_AddStringToObject(d, "state",
                                job->done ? (job->error.empty() ? "done" : "failed") : "pending");
        if (job->done) {
          if (!job->error.empty())
            cJSON_AddStringToObject(d, "error", job->error.c_str());
          else {
            auto *v = cJSON_Parse(job->result.c_str());
            if (v)
              cJSON_AddItemToObject(d, "result", v);
          }
        }
        return r;
      }
    return response("unknown_job");
  }
  while (jobs.size() >= 16) {
    auto p = std::find_if(jobs.begin(), jobs.end(), [](auto &v) { return v->done; });
    if (p == jobs.end())
      return response("busy");
    jobs.erase(p);
  }
  auto job = std::make_shared<Job>();
  job->id = ++next_job;
  job->conn = conn;
  job->json = dump(j);
  auto *w = new Work;
  w->job = job;
  if (xQueueSend(queue, &w, 0) != pdTRUE) {
    delete w;
    return response("busy");
  }
  jobs.push_back(job);
  auto *r = response();
  cJSON_AddNumberToObject(cJSON_GetObjectItem(r, "data"), "job_id", job->id);
  return r;
}
} // namespace ya::gallery
