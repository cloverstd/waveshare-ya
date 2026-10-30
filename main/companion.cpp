#include "companion.hpp"
#include "rich_limits.hpp"
#include "power.hpp"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"
#include "zlib.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>
namespace ya {
namespace {
SemaphoreHandle_t view_mutex;
std::shared_ptr<RichMessage> current;
bool loaded = false;
std::atomic<bool> receiving{false};
struct Record {
  uint32_t magic = 0x59414931, generation = 0, height = 0, text = 0, packed = 0;
  uint8_t digest[32]{};
  uint32_t checksum = 0;
};
struct Upload {
  int conn = -1;
  std::string id, sha;
  Record record;
  size_t offset = 0;
  int64_t started = 0, touched = 0;
  std::shared_ptr<uint8_t> bytes;
} upload;
std::shared_ptr<uint8_t> memory(size_t n) {
  return {static_cast<uint8_t *>(heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free};
}
std::string string(const cJSON *j, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsString(v) ? v->valuestring : "";
}
size_t number(const cJSON *j, const char *key) {
  auto *v = cJSON_GetObjectItemCaseSensitive(j, key);
  return cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 10485760 &&
         v->valuedouble == v->valueint ? size_t(v->valueint) : SIZE_MAX;
}
bool mounted() { Guard g(state_mutex); return state.flash_mounted; }
uint32_t record_checksum(const Record &r) { return crc32(0, reinterpret_cast<const Bytef *>(&r), offsetof(Record, checksum)); }
bool valid(const Record &r) { return r.magic == 0x59414931 && r.checksum == record_checksum(r) && rich::dimensions(r.height, r.text, r.packed); }
std::shared_ptr<RichMessage> decode(const Record &r, const uint8_t *packed) {
  if (!valid(r)) return {};
  uint8_t digest[32]; mbedtls_sha256(packed, r.packed, digest, 0);
  if (memcmp(digest, r.digest, 32)) return {};
  size_t pixel_size = rich::WIDTH * r.height * 2;
  size_t n = r.text + pixel_size;
  auto bytes = memory(n);
  if (!bytes || !rich::unpack(packed, r.packed, r.height, r.text, bytes.get(), n)) return {};
  auto message = std::make_shared<RichMessage>();
  message->revision = r.generation; message->height = r.height;
  message->text.assign(reinterpret_cast<char *>(bytes.get()), r.text);
  memmove(bytes.get(), bytes.get() + r.text, pixel_size);
  message->pixels = bytes;
  return message;
}
std::shared_ptr<RichMessage> read_record(const char *path) {
  FILE *f = fopen(path, "rb"); if (!f) return {};
  Record r;
  if (fread(&r, 1, sizeof r, f) != sizeof r || !valid(r)) { fclose(f); return {}; }
  auto bytes = memory(r.packed);
  bool ok = bytes && fread(bytes.get(), 1, r.packed, f) == r.packed && fgetc(f) == EOF;
  fclose(f);
  return ok ? decode(r, bytes.get()) : nullptr;
}
bool verify_record(const char *path, const Record &expected) {
  FILE *f = fopen(path, "rb"); if (!f) return false;
  Record r; bool ok = fread(&r, 1, sizeof r, f) == sizeof r && !memcmp(&r, &expected, sizeof r);
  mbedtls_sha256_context hash; mbedtls_sha256_init(&hash); mbedtls_sha256_starts(&hash, 0);
  uint8_t block[512], digest[32];
  for (size_t offset = 0; ok && offset < expected.packed; offset += sizeof block) {
    size_t n = std::min(sizeof block, size_t(expected.packed - offset));
    ok = fread(block, 1, n, f) == n;
    if (ok) mbedtls_sha256_update(&hash, block, n);
  }
  mbedtls_sha256_finish(&hash, digest); mbedtls_sha256_free(&hash);
  ok = ok && fgetc(f) == EOF && !memcmp(digest, expected.digest, sizeof digest);
  fclose(f); return ok;
}
cJSON *result(const char *error = nullptr) {
  auto *j = cJSON_CreateObject();
  cJSON_AddStringToObject(j, "status", error ? "failed" : "succeeded");
  if (error) { auto *e = cJSON_AddObjectToObject(j, "error"); cJSON_AddStringToObject(e, "code", error); }
  else cJSON_AddObjectToObject(j, "data");
  return j;
}
void cancel() { upload = {}; receiving = false; }
}
void companion_init() { view_mutex = xSemaphoreCreateMutex(); if (!view_mutex) abort(); }
std::shared_ptr<const RichMessage> companion_snapshot() { Guard g(view_mutex); return current; }
bool companion_busy() { return receiving; }
void companion_disconnect(int conn) { if (upload.conn == conn) cancel(); }
void companion_tick() {
  if (upload.conn >= 0) {
    if (now_ms() - upload.touched > 45000 || now_ms() - upload.started > 1200000 || ota_busy) cancel();
    else power_hold(2000);
  }
  if (!loaded && mounted()) {
    Guard storage(storage_mutex);
    auto a = read_record("/spiffs/info0"), b = read_record("/spiffs/info1");
    Guard g(view_mutex); current = !a || (b && b->revision > a->revision) ? b : a; loaded = true;
  }
}
cJSON *companion_request(int conn, const cJSON *j) {
  auto op = string(j, "op"), id = string(j, "transfer");
  if (op == "cancel") { if (upload.conn == conn && upload.id == id) cancel(); return result(); }
  if (ota_busy) return result("busy");
  if (op == "begin") {
    if (upload.conn >= 0) return result("busy");
    auto height = number(j, "height"), text = number(j, "text_bytes"), packed = number(j, "packed_bytes");
    auto sha = string(j, "sha256");
    if (id.empty() || id.size() > 64 || sha.size() != 64 || !rich::dimensions(height, text, packed)) return result("invalid_size");
    if (!mounted()) return result("storage_unavailable");
    auto bytes = memory(packed); if (!bytes) return result("no_memory");
    upload.conn = conn; upload.id = id; upload.sha = sha; upload.bytes = bytes;
    upload.record.height = height; upload.record.text = text; upload.record.packed = packed;
    upload.started = upload.touched = now_ms(); receiving = true; power_hold(2000);
    return result();
  }
  if (upload.conn != conn || upload.id != id) return result("unknown_transfer");
  upload.touched = now_ms(); power_hold(2000);
  if (op == "chunk") {
    auto offset = number(j, "offset"); auto value = string(j, "bytes");
    uint8_t bytes[1536]; size_t n = 0;
    if (value.size() > 2048 || mbedtls_base64_decode(bytes, sizeof bytes, &n,
        reinterpret_cast<const unsigned char *>(value.data()), value.size()) || !n ||
        offset > upload.offset || offset > upload.record.packed || n > upload.record.packed - offset)
      return result("invalid_chunk");
    if (offset < upload.offset) {
      if (offset + n > upload.offset || memcmp(upload.bytes.get() + offset, bytes, n)) return result("offset_mismatch");
    } else { memcpy(upload.bytes.get() + offset, bytes, n); upload.offset += n; }
    auto *r = result(); cJSON_AddNumberToObject(cJSON_GetObjectItem(r, "data"), "offset", upload.offset); return r;
  }
  if (op != "commit") return result("unsupported");
  if (upload.offset != upload.record.packed) return result("incomplete");
  auto &r = upload.record;
  mbedtls_sha256(upload.bytes.get(), r.packed, r.digest, 0);
  char sha[65]; for (unsigned i = 0; i < 32; ++i) sprintf(sha + i * 2, "%02x", r.digest[i]);
  if (upload.sha != sha) { cancel(); return result("checksum_mismatch"); }
  auto previous = companion_snapshot(); r.generation = previous ? previous->revision + 1 : 1;
  r.checksum = record_checksum(r);
  auto message = decode(r, upload.bytes.get());
  if (!message) { cancel(); return result("invalid_payload"); }
  bool ok = false;
  {
    Guard mutation(mutation_mutex); Guard storage(storage_mutex);
    if (!ota_busy && mounted()) {
      const char *path = r.generation % 2 ? "/spiffs/info1" : "/spiffs/info0";
      FILE *f = fopen(path, "wb");
      if (f) {
        ok = fwrite(&r, 1, sizeof r, f) == sizeof r && fwrite(upload.bytes.get(), 1, r.packed, f) == r.packed;
        ok = fflush(f) == 0 && ok;
        ok = fsync(fileno(f)) == 0 && ok;
        ok = fclose(f) == 0 && ok;
        if (ok) ok = verify_record(path, r);
      }
    }
  }
  if (!ok) { cancel(); return result("storage_error"); }
  { Guard g(view_mutex); current = message; }
  cancel(); return result();
}
}
