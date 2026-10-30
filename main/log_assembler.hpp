#pragma once
#include "log_buffer.hpp"
namespace ya {
// Fixed task slots, called under the log buffer try-lock. No allocation or I/O.
struct LogAssembler {
  struct Partial { uintptr_t task = 0; int level = 0; size_t size = 0; uint64_t at = 0; bool cut = false; char text[LOG_TEXT_SIZE]{}; };
  Partial partial[8]{};
  void flush(Partial &p, LogBuffer &out) {
    if (p.level && p.size) { p.text[log_utf8_prefix(p.text, p.size)] = 0; out.append(p.level, p.text); }
    p = {};
  }
  void expire(uint64_t now, LogBuffer &out) {
    for (auto &p : partial) if (p.task && now - p.at >= 1000) flush(p, out);
  }
  void feed(uintptr_t task, const char *text, uint64_t now, int threshold, LogBuffer &out) {
    expire(now, out);
    Partial *p = nullptr;
    for (auto &v : partial) if (v.task == task) { p = &v; break; }
    int level = log_line_level(text);
    if (level) {
      if (p) flush(*p, out);
      if (!log_accepts(threshold, level)) return;
      if (!p) for (auto &v : partial) if (!v.task) { p = &v; break; }
      if (!p) { p = &partial[0]; for (auto &v : partial) if (v.at < p->at) p = &v; flush(*p, out); }
      p->task = task; p->level = level;
    }
    if (!p) return;
    p->at = now;
    for (; *text; ++text) {
      if (*text == '\n') { flush(*p, out); return; }
      if (p->size < LOG_TEXT_SIZE - 1) p->text[p->size++] = *text;
      else p->cut = true;
    }
  }
  void clear() { for (auto &p : partial) p = {}; }
};
}
