#pragma once
#include "ya.hpp"
#include "cJSON.h"
#include <memory>
#include <string>
namespace ya {
struct RichMessage {
  uint32_t revision = 0;
  unsigned height = 0;
  std::string text;
  std::shared_ptr<uint8_t> pixels;
};
void companion_init();
cJSON *companion_request(int conn, const cJSON *request); // data worker only
void companion_disconnect(int conn);
void companion_tick();
bool companion_busy();
std::shared_ptr<const RichMessage> companion_snapshot();
bool companion_send_logs(); // queued through the existing data worker
}
