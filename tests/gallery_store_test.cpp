// Exercise the production service's persistence and request paths with a host
// filesystem/NVS adapter. Radio, decoding, scheduling and power are not simulated.
#define YA_GALLERY_HOST_TEST
#include "../main/gallery.cpp"
#include <cassert>
#include <iostream>
using namespace ya::gallery;
static cJSON *call(const char *json, std::string &error) {
  auto j = std::make_shared<Job>();
  j->conn = 1;
  j->json = json;
  error.clear();
  return handle(j, error);
}
int main(int argc, char **argv) {
  assert(argc == 2);
  host::root = argv[1];
  std::filesystem::create_directories(host::root);
  mutex = xSemaphoreCreateMutex();
  assert(load());
  std::string error;
  auto *r =
      call(R"({"resource":"gallery.albums","op":"create","name":"旅行","source":"local"})", error);
  assert(r && error.empty());
  auto aid = str(r, "album");
  cJSON_Delete(r);
  assert(catalog.albums.size() == 1);
  auto oldroot = host::roots["ota_0"];
  host::roots["ota_1"] = oldroot;
  Catalog next = catalog;
  next.albums[0].name = "家人";
  assert(save(next));
  assert(host::roots["ota_0"] != oldroot);
  assert(std::filesystem::exists(path("c", oldroot))); // rollback root retained
  std::string active = host::roots["ota_0"];
  host::fail_commit = true;
  next = catalog;
  next.albums[0].name = "提交失败";
  assert(!save(next));
  assert(catalog.albums[0].name == "家人");
  assert(host::roots["ota_0"] == active);
  host::fail_commit = false;
  collect();
  // An object referenced by the other slot survives; unrelated files survive.
  Bytes photo{1, 2, 3};
  auto digest = hash(photo.data(), photo.size());
  {
    FILE *f = fopen((host::root + "/unrelated").c_str(), "wb");
    fputs("keep", f);
    fclose(f);
  }
  assert(write_object("p", digest, photo));
  next = catalog;
  next.albums[0].photos.push_back({"one", "", "", digest});
  assert(save(next));
  auto withphoto = host::roots["ota_0"];
  host::roots["ota_1"] = withphoto;
  next = catalog;
  next.albums[0].photos.clear();
  assert(save(next));
  assert(std::filesystem::exists(path("p", digest)));
  host::roots["ota_1"] = host::roots["ota_0"];
  collect();
  assert(!std::filesystem::exists(path("p", digest)));
  assert(std::filesystem::exists(host::root + "/unrelated"));
  // Replay of identical upload chunks works; conflicting replay is rejected.
  auto begin = std::string(R"({"resource":"gallery.transfer","op":"begin","album":")") + aid +
               R"(","transfer":"upload","size":3,"sha256":")" + digest + R"("})";
  r = call(begin.c_str(), error);
  assert(r);
  cJSON_Delete(r);
  for (int i = 0; i < 2; ++i) {
    r = call(
        R"({"resource":"gallery.transfer","op":"chunk","transfer":"upload","offset":0,"bytes":"AQID"})",
        error);
    assert(r);
    cJSON_Delete(r);
    assert(upload.bytes == photo);
  }
  r = call(
      R"({"resource":"gallery.transfer","op":"chunk","transfer":"upload","offset":0,"bytes":"BAUG"})",
      error);
  assert(!r && error == "offset_mismatch");
  r = call(R"({"resource":"gallery.transfer","op":"commit","transfer":"upload"})", error);
  assert(r && catalog.albums[0].photos.size() == 1);
  cJSON_Delete(r);
  // Storage full preserves catalog and both NVS references.
  active = host::roots["ota_0"];
  host::capacity = 512 * 1024;
  next = catalog;
  next.albums[0].name = "空间不足";
  assert(!save(next));
  assert(host::roots["ota_0"] == active);
  host::capacity = 3 * 1024 * 1024;
  // Corrupt catalog never silently initializes or deletes existing objects.
  auto catalog_path = path("c", active);
  FILE *f = fopen(catalog_path.c_str(), "wb");
  fputs("corrupt", f);
  fclose(f);
  loaded = false;
  broken = false;
  assert(!load() && broken);
  collect();
  assert(std::filesystem::exists(catalog_path));
  std::cout << "gallery production persistence, rollback roots, chunks, failed commit/full/corrupt "
               "recovery passed\n";
}
