#include "../main/ota_web_session.hpp"
#include <cassert>
#include <string>

using ya::ota_web::Session;
int main() {
  const char *first = "0123456789abcdef0123456789abcdef";
  const char *second = "fedcba9876543210fedcba9876543210";
  const auto auth = std::string("Bearer ") + first;
  const auto replacement = std::string("Bearer ") + second;
  Session s;
  assert(!s.valid(0) && !s.begin_upload(0));
  assert(!s.open(nullptr, 0) && !s.open("short", 0));
  assert(s.open(first, 100));
  assert(s.authorized(auth.c_str(), 100));
  assert(!s.authorized(first, 100));
  assert(!s.authorized((auth + "x").c_str(), 100));
  assert(!s.authorized(replacement.c_str(), 100));
  assert(!s.close_if_idle(600099, true));
  // The exact deadline rejects a new upload and allows shutdown.
  assert(!s.authorized(auth.c_str(), 600100));
  assert(!s.begin_upload(600100));
  assert(s.close_if_idle(600100, true));

  // An upload admitted immediately before expiry pins the server through
  // queueing, transfer, verification, response and async-request cleanup.
  assert(s.open(first, 100));
  assert(s.begin_upload(600099));
  assert(!s.begin_upload(600099));
  assert(!s.open(second, 600099));
  assert(!s.close_if_idle(600100, true));
  assert(s.uploading());
  assert(!s.authorized(auth.c_str(), 600100));
  s.complete_upload();
  assert(s.close_if_idle(600101, true));

  // Disconnect invalidates the credential, but must not free a request which
  // the worker still owns. Reconnection never reactivates the old credential.
  assert(s.open(first, 1000));
  assert(s.begin_upload(1001));
  assert(!s.close_if_idle(1002, false));
  assert(!s.authorized(auth.c_str(), 1003));
  s.complete_upload();
  assert(s.close_if_idle(1004, true));

  // Setup/queue failure releases the pin without consuming the whole session.
  assert(s.open(first, 2000));
  assert(s.begin_upload(2001));
  s.complete_upload();
  assert(!s.close_if_idle(2002, true));
  assert(s.begin_upload(2003));
  s.complete_upload();
  assert(s.open(second, 2004));
  assert(!s.authorized(auth.c_str(), 2005));
  assert(s.authorized(replacement.c_str(), 2005));

  // Repeated start/expiry cycles and long uptime must not retain ownership or
  // overflow a 32-bit milliseconds counter.
  const int64_t base = int64_t{1} << 40;
  for (int i = 0; i < 100; ++i) {
    auto now = base + int64_t{i} * 700000;
    assert(s.open(first, now));
    assert(s.authorized(auth.c_str(), now));
    assert(s.close_if_idle(now + Session::lifetime_ms, true));
    assert(!s.uploading() && !s.valid(now + Session::lifetime_ms));
  }
}
