#include "../main/log_buffer.hpp"
#include <cassert>
#include <cstdio>
#include <string>
using namespace ya;
int main() {
  assert(LOG_DEFAULT_LEVEL == 2);
  for (int threshold = 0; threshold <= 5; ++threshold)
    for (int level = 0; level <= 6; ++level)
      assert(log_accepts(threshold, level) == (level > 0 && level <= threshold));
  assert(!log_level_valid(-1) && !log_level_valid(6));
  assert(!log_accepts(-1, 1) && !log_accepts(6, 1));
  const char *letters = "EWIDV";
  for (int level = 1; level <= 5; ++level) {
    std::string line(1, letters[level - 1]); line += " (123) test: message\n";
    assert(log_line_level(line.c_str()) == level);
    assert(log_line_level(("\033[0;33m" + line).c_str()) == level);
  }
  for (auto *line : {"", "plain text", "YA_JSON {}", "Warning", "\033[33", "E"})
    assert(log_line_level(line) == 0);
  assert(log_line_level(nullptr) == 0);
  LogBuffer log;
  for (unsigned i = 0; i < LOG_CAPACITY + 25; ++i)
    log.append(2, std::to_string(i).c_str());
  assert(log.count == 100 && log.overwritten == 25 && log.revision == 125);
  for (unsigned i = 0; i < log.count; ++i) {
    assert(log.at(i).level == 2);
    assert(log.at(i).text == std::to_string(i + 25));
  }
  auto frozen = log;
  log.append(1, "new error\r\n");
  assert(std::string(log.at(99).text) == "new error" && frozen.at(99).text == std::to_string(124));
  auto revision = log.revision;
  log.append(0, "invalid"); log.append(6, "invalid"); log.append(2, "");
  assert(log.revision == revision);
  // Test every cut position through Chinese, emoji and ASCII using the real
  // vsnprintf-style truncated buffer, where the missing byte is already NUL.
  std::string unicode = "W (1) test: 中文🙂abc";
  for (size_t capacity = 1; capacity <= unicode.size() + 2; ++capacity) {
    char text[64];
    int total = snprintf(text, capacity, "%s", unicode.c_str());
    log_clean(text, capacity, total);
    assert(unicode.compare(0, strlen(text), text) == 0);
    for (size_t i = 0; i < strlen(text);) {
      auto c = static_cast<unsigned char>(text[i]);
      size_t width = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
      assert(i + width <= strlen(text)); i += width;
    }
  }
  char ansi[] = "\033[0;33mW (1) test: warning\033[0m\r\n";
  log_clean(ansi, sizeof(ansi), strlen(ansi));
  assert(std::string(ansi) == "W (1) test: warning\n");
  log.append(2, (std::string(254, 'x') + "中文").c_str());
  assert(strlen(log.at(99).text) == 254);
  log.clear();
  assert(log.count == 0 && log.overwritten == 0 && log.revision > revision);
  log.append(3, "after clear");
  assert(log.count == 1 && std::string(log.at(0).text) == "after clear");
}
