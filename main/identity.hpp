#pragma once
#include <cstdint>
#include <cstring>
namespace ya {
// Our control and update protocols use flat objects; reject nesting before the recursive parser.
inline bool flat_json_object(const char *text, size_t length) {
  auto whitespace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t pos = 0;
  while (pos < length && whitespace(text[pos]))
    pos++;
  if (pos == length || text[pos++] != '{')
    return false;
  bool quoted = false, escaped = false, finished = false;
  for (; pos < length; pos++) {
    char c = text[pos];
    if (finished) {
      if (!whitespace(c))
        return false;
      continue;
    }
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[' || c == ']')
      return false;
    else if (c == '}')
      finished = true;
  }
  return finished && !quoted && !escaped;
}
struct FirmwareIdentity {
  uint32_t magic;
  char board[40];
  char layout[16];
};
constexpr uint32_t IDENTITY_MAGIC = 0x59414142;
inline bool identity_valid(const FirmwareIdentity &i) {
  return i.magic == IDENTITY_MAGIC &&
         !std::strncmp(i.board, "esp32-s3-touch-amoled-2.16", sizeof(i.board)) &&
         !std::strncmp(i.layout, "ab6-v1", sizeof(i.layout));
}
inline bool update_power_allowed(bool valid, bool fresh, bool usb, bool present, int percent) {
  return valid && fresh && (usb || (present && percent >= 30 && percent <= 100));
}
} // namespace ya
