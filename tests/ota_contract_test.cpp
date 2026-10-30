#include "../main/ota_contract.hpp"
#include <cassert>
#include <iostream>
#include <string>

static const std::string valid =
    R"JSON({"product":"waveshare-ya","board":"esp32-s3-touch-amoled-2.16","layout":"ab6-v1","version":"0.2.0","url":"https://ya.cloverstd.com/firmware/waveshare-ya/0.2.0/waveshare-ya-0.2.0.bin","size":2000000,"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","notes":"更新"})JSON";
static std::string changed(const std::string &old_value, const std::string &new_value) {
  auto body = valid;
  auto pos = body.find(old_value);
  assert(pos != std::string::npos);
  body.replace(pos, old_value.size(), new_value);
  return body;
}
static unsigned cases = 0;
static bool parse(const std::string &body, ya::ota_contract::Release &release) {
  ++cases;
  return ya::ota_contract::parse(body.data(), body.size(), release, "waveshare-ya",
                                 "esp32-s3-touch-amoled-2.16", "ab6-v1", 0x600000);
}
static void rejects(const std::string &body) {
  ya::ota_contract::Release release;
  std::strcpy(release.version, "keep");
  assert(!parse(body, release));
  assert(std::string(release.version) == "keep");
}
int main() {
  ya::ota_contract::Release release;
  assert(parse(valid, release));
  assert(std::string(release.version) == "0.2.0" && release.size == 2000000);
  assert(std::string(release.notes) == "更新");
  assert(parse(changed(",\"notes\":\"更新\"", ""), release));
  assert(release.notes[0] == 0);
  assert(parse(changed("\"notes\":\"更新\"", "\"extra\":true"), release));
  for (auto size : {"512", "6291456"})
    assert(parse(changed("2000000", size), release));
  for (auto size : {"511", "6291457", "-1", "0", "512.5", "true", "\"2000000\"", "1e999"})
    rejects(changed("2000000", size));
  rejects(changed("waveshare-ya\"", "another-product\""));
  rejects(changed("esp32-s3-touch-amoled-2.16", "esp32-s3-touch-amoled-1.8"));
  rejects(changed("ab6-v1", "ab3-v0"));
  rejects(changed("\"version\":\"0.2.0\"", "\"version\":\"\""));
  rejects(changed("\"version\":\"0.2.0\"", "\"version\":\"../escape\""));
  rejects(changed("\"version\":\"0.2.0\"", "\"version\":\"" + std::string(32, 'a') + "\""));
  rejects(changed("\"version\":\"0.2.0\"", "\"Version\":\"0.2.0\""));
  rejects(changed("\"version\":\"0.2.0\"", "\"version\":\"0.2.0\",\"version\":\"0.1.0\""));
  rejects(changed("\"notes\":\"更新\"", "\"notes\":[]"));
  rejects(changed("\"notes\":\"更新\"", "\"notes\":{}"));
  rejects(changed("\"notes\":\"更新\"", "\"notes\":null"));
  rejects(changed("更新", std::string(256, 'a')));
  assert(parse(changed("更新", std::string(255, 'a')), release));
  std::string utf8;
  for (int i = 0; i < 85; ++i)
    utf8 += "字";
  assert(parse(changed("更新", utf8), release));
  rejects(changed("更新", utf8 + "字"));
  rejects(changed(std::string(64, 'a'), std::string(64, 'A')));
  rejects(changed(std::string(64, 'a'), std::string(63, 'a')));
  rejects(changed("https://ya.cloverstd.com", "http://ya.cloverstd.com"));
  rejects(changed("https://ya.cloverstd.com", "https://user:pass@ya.cloverstd.com"));
  rejects(changed(".bin\"", ".bin#fragment\""));
  rejects(changed("https://ya.cloverstd.com", "https:///missing-host"));
  rejects(changed("https://ya.cloverstd.com", "https://bad host"));
  rejects(changed("https://ya.cloverstd.com", "https://" + std::string(384, 'a')));
  // A query string is permitted, including URL-encoded authorization provided by the publisher.
  assert(parse(changed(".bin\"", ".bin?source=stable\""), release));
  rejects(changed("0.2.0\"", "0.2.0\\u0000hidden\""));
  assert(parse(changed("更新", "literal \\\\u0000"), release));
  rejects(changed("更新", std::string("x\0y", 3)));
  rejects(valid + "{}");
  rejects("[" + valid + "]");
  rejects("{}");
  rejects("");
  rejects(valid + std::string(4096 - valid.size(), ' '));
  assert(parse(valid + std::string(4095 - valid.size(), ' '), release));
  std::cout << "OTA contract cases passed: " << cases << "\n";
}
