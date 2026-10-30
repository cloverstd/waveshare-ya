#pragma once
#include <cstdint>
#include <cstring>

namespace ya::ota_web {
// All access is serialized by ota_mutex. A queued async upload owns the server
// until its HTTP request has been completed, including setup/error cleanup.
class Session {
  char token_[33]{};
  int64_t expires_ = 0;
  bool uploading_ = false;

public:
  static constexpr int64_t lifetime_ms = 600000;
  bool open(const char *token, int64_t now) {
    if (uploading_ || !token || std::strlen(token) != 32)
      return false;
    std::memcpy(token_, token, sizeof token_);
    expires_ = now + lifetime_ms;
    return true;
  }
  bool valid(int64_t now) const { return token_[0] && now < expires_; }
  bool authorized(const char *header, int64_t now) const {
    if (!valid(now) || !header || std::strlen(header) != 39 ||
        std::strncmp(header, "Bearer ", 7))
      return false;
    unsigned diff = 0;
    for (unsigned i = 0; i < 32; ++i)
      diff |= header[i + 7] ^ token_[i];
    return diff == 0;
  }
  bool begin_upload(int64_t now) {
    if (uploading_ || !valid(now))
      return false;
    uploading_ = true;
    return true;
  }
  void complete_upload() { uploading_ = false; }
  bool uploading() const { return uploading_; }
  void invalidate() { token_[0] = 0; expires_ = 0; }
  bool close_if_idle(int64_t now, bool connected) {
    if (connected && valid(now))
      return false;
    invalidate(); // Reject new uploads before releasing the lifecycle lock.
    return !uploading_;
  }
};
} // namespace ya::ota_web
