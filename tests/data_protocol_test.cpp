#include "../main/data_protocol.hpp"
#include <cassert>
using namespace ya::protocol;
int main() {
  // A maximum-sized browser frame must finish before its queued ACK expires;
  // an 8-second provider fetch must also fit inside the response deadline.
  constexpr int64_t slow_frame_ms = ((LIMIT + 4 + 19) / 20) * 100;
  static_assert(ACK_TIMEOUT_MS > slow_frame_ms + 1500);
  static_assert(RESPONSE_TIMEOUT_MS > slow_frame_ms + 8000 + 1500);
  static_assert(REQUEST_TIMEOUT_MS > RESPONSE_TIMEOUT_MS + 5000);
  static_assert(REQUEST_TIMEOUT_MS < 55000);
  assert(!ya::protocol::handshake_expired(false,15001));
  assert(!ya::protocol::handshake_expired(false,120000));
  assert(ya::protocol::handshake_expired(false,120001));
  assert(!ya::protocol::handshake_expired(true,30000));
  assert(ya::protocol::handshake_expired(true,30001));

  Frame f;
  std::string out;
  auto b = frame("{\"text\":\"天气\"}");
  for (size_t i = 0; i < b.size(); i++) {
    assert(f.feed((const uint8_t *)b.data() + i, 1, out));
    if (i + 1 < b.size())
      assert(out.empty());
  }
  assert(out == "{\"text\":\"天气\"}");
  assert(bounded_json(out));
  auto huge = frame(std::string(4097, 'x'));
  assert(!f.feed((const uint8_t *)huge.data(), 4, out));
  auto zero = frame("");
  assert(!f.feed((const uint8_t *)zero.data(), 4, out));
  auto both = frame("{}") + frame("{}");
  assert(!f.feed((const uint8_t *)both.data(), both.size(), out));
  assert(!bounded_json(std::string("{\"a\":\"") + char(0xc0) + char(0x80) + "\"}"));
  assert(!bounded_json("{\"a\":[[[[[[0]]]]]]}"));
  assert(bounded_json("{\"a\":[[[[0]]]]}"));
  assert(uuid("b09a2528-722b-461d-80cc-1bbc2df44ab8"));
  assert(!uuid("bad"));
  assert(!better(-60, -55, true, true));
  assert(better(-60, -52, true, true));
  assert(!better(-60, -40, true, false));
  assert(better(-60, -90, false, false));
}
