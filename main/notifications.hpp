#pragma once
#include <cstdint>
#include <cstring>
#include <algorithm>
namespace ya {
enum class NoticeKind : uint8_t { SYSTEM, WIFI, BLE, OTA };
struct Notification {
  uint32_t id = 0, boot = 0;
  int64_t epoch = 0, uptime = 0;
  uint16_t repeats = 1;
  NoticeKind kind = NoticeKind::SYSTEM;
  char text[160]{};
};
struct NotificationLog {
  uint32_t schema = 1, next = 1, read = 0, revision = 0, count = 0;
  Notification entries[10]{}; // oldest first; bounded independently of Status
  void append(const char *text, NoticeKind kind, uint32_t boot, int64_t uptime, int64_t epoch) {
    if (!text || !*text) return;
    Notification *p = count ? &entries[count - 1] : nullptr;
    bool merge = p && p->kind == kind && p->boot == boot && uptime - p->uptime < 5000 &&
                 !strcmp(p->text, text);
    if (!merge) {
      if (count == 10) { std::move(entries + 1, entries + 10, entries); --count; }
      p = &entries[count++]; *p = {};
      size_t n = std::min(strlen(text), sizeof(p->text) - 1);
      while (n && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
      memcpy(p->text, text, n);
    } else if (p->repeats < 65535) ++p->repeats;
    p->id = next++; p->kind = kind; p->boot = boot; p->uptime = uptime; p->epoch = epoch;
    ++revision;
  }
  void erase_through(uint32_t id) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) if (entries[i].id > id) entries[n++] = entries[i];
    count = n; ++revision;
  }
  unsigned unread() const {
    unsigned n = 0; for (uint32_t i = 0; i < count; ++i) n += entries[i].id > read; return n;
  }
  bool correct_time(uint32_t boot, int64_t uptime, int64_t epoch) {
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) if (!entries[i].epoch && entries[i].boot == boot) {
      entries[i].epoch = epoch - (uptime - entries[i].uptime) / 1000; changed = true;
    }
    if (changed) ++revision;
    return changed;
  }
};
void notifications_init();
void notifications_add(const char *, NoticeKind = NoticeKind::SYSTEM, bool toast = true);
void notifications_snapshot(NotificationLog &);
void notifications_read(uint32_t through);
bool notifications_clear();
int notifications_clear_result(); // 0=pending/none, 1=success, -1=failure
void notifications_tick();
bool notifications_checkpoint();
}
