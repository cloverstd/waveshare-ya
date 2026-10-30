#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace ya::battery_stats {
enum class Mode { UNKNOWN, DISCHARGING, CHARGING, USB, FULL };
enum class Phase { UNAVAILABLE, LEARNING, READY, NO_PROGRESS, CONFIRMING_FULL, FULL };

inline const char *name(Mode mode) {
  switch (mode) {
  case Mode::DISCHARGING:
    return "discharging";
  case Mode::CHARGING:
    return "charging";
  case Mode::USB:
    return "usb_idle";
  case Mode::FULL:
    return "full";
  default:
    return "unknown";
  }
}
inline const char *name(Phase phase) {
  switch (phase) {
  case Phase::LEARNING:
    return "learning";
  case Phase::READY:
    return "ready";
  case Phase::NO_PROGRESS:
    return "no_progress";
  case Phase::CONFIRMING_FULL:
    return "confirming_full";
  case Phase::FULL:
    return "full";
  default:
    return "unavailable";
  }
}
struct Sample {
  int64_t at_ms;
  int percent;
  bool valid, present, usb, charging, discharging, full;
};
struct Summary {
  Mode mode = Mode::UNKNOWN;
  Phase phase = Phase::UNAVAILABLE;
  int64_t session_seconds = 0, recent_seconds = 0;
  int change_percent = 0;
  // Rates are percentage points/hour, not current or energy measurements.
  float session_rate = -1, recent_rate = -1;
  int remaining_minutes = -1; // unknown; zero is reserved for PMU-confirmed full
};

// Bounded RAM-only history. Called by the existing services worker, never GUI.
// A reboot/deep sleep starts a new monitoring segment; no Flash writes or clock sync.
class Tracker {
public:
  Summary update(const Sample &s) {
    const auto mode = classify(s);
    if (mode != Mode::DISCHARGING && mode != Mode::CHARGING) {
      started_ = false;
      Summary out;
      out.mode = mode;
      if (mode == Mode::FULL) {
        out.phase = Phase::FULL;
        out.remaining_minutes = 0;
      }
      return out;
    }
    // Do not bridge missing telemetry, supply changes, clock resets or gauge corrections.
    if (!started_ || mode != mode_ || s.at_ms <= last_at_ || s.at_ms - last_at_ > MAX_GAP_MS ||
        std::abs(s.percent - last_raw_) > 3 ||
        (mode == Mode::CHARGING ? best_ - s.percent : s.percent - best_) > 1) {
      begin(s, mode);
    } else {
      const int next =
          mode == Mode::CHARGING ? std::max(best_, s.percent) : std::min(best_, s.percent);
      if (next != best_) {
        best_ = next;
        progress_at_ = s.at_ms;
      }
      last_at_ = s.at_ms;
      last_raw_ = s.percent;
      if (s.at_ms - newest().at_ms >= POINT_MS)
        append({s.at_ms, best_});
      while (count_ > 1 && s.at_ms - oldest().at_ms > WINDOW_MS) {
        head_ = (head_ + 1) % CAPACITY;
        --count_;
      }
    }
    Summary out;
    out.mode = mode;
    out.phase = Phase::LEARNING;
    out.session_seconds = (s.at_ms - began_at_) / 1000;
    out.change_percent = std::abs(best_ - began_percent_);
    if (out.session_seconds >= MIN_MS / 1000 && out.change_percent >= MIN_CHANGE)
      out.session_rate = out.change_percent * 3600.0 / out.session_seconds;
    const auto span = s.at_ms - oldest().at_ms;
    out.recent_seconds = span / 1000;
    if (mode == Mode::CHARGING && s.percent == 100) {
      // SOC=100 is not the PMU's charge-complete indication.
      out.phase = Phase::CONFIRMING_FULL;
      return out;
    }
    if (mode == Mode::CHARGING && s.at_ms - progress_at_ >= STALL_MS) {
      out.phase = Phase::NO_PROGRESS;
      return out;
    }
    const auto change = std::abs(best_ - oldest().percent);
    if (span < MIN_MS || change < MIN_CHANGE)
      return out;
    out.recent_rate = change * 3600000.0 / span;
    double minutes;
    if (mode == Mode::CHARGING) {
      // Explicit heuristic, not a calibrated AXP2101 charge curve: account for
      // slower finishing stages, then continually refit to observed progress.
      const auto work = charge_work(best_) - charge_work(oldest().percent);
      minutes = (charge_work(100) - charge_work(s.percent)) * span / work / 60000.0;
    } else {
      minutes = best_ * 60.0 / out.recent_rate;
    }
    if (!std::isfinite(minutes) || minutes < 0 || minutes > 7 * 24 * 60)
      return out;
    // Avoid a false precision countdown from integer SOC; never show 0 while charging.
    out.remaining_minutes = std::max(5, int(std::ceil(minutes / 5.0)) * 5);
    out.phase = Phase::READY;
    return out;
  }

private:
  static constexpr size_t CAPACITY = 61;
  static constexpr int64_t POINT_MS = 120000, WINDOW_MS = 7200000;
  static constexpr int64_t MIN_MS = 300000, MAX_GAP_MS = 90000, STALL_MS = 1200000;
  static constexpr int MIN_CHANGE = 2;
  struct Point {
    int64_t at_ms;
    int percent;
  } points_[CAPACITY]{};
  size_t head_ = 0, count_ = 0;
  bool started_ = false;
  Mode mode_ = Mode::UNKNOWN;
  int began_percent_ = 0, best_ = 0, last_raw_ = 0;
  int64_t began_at_ = 0, last_at_ = 0, progress_at_ = 0;

  static Mode classify(const Sample &s) {
    if (!s.valid || !s.present || s.percent < 0 || s.percent > 100 || s.at_ms < 0)
      return Mode::UNKNOWN;
    if (s.usb) {
      if (s.charging && !s.discharging)
        return Mode::CHARGING;
      if (s.full && !s.charging && !s.discharging)
        return Mode::FULL;
      return Mode::USB;
    }
    return s.discharging && !s.charging ? Mode::DISCHARGING : Mode::UNKNOWN;
  }
  static double charge_work(int percent) {
    return std::min(percent, 80) + std::clamp(percent - 80, 0, 10) * 1.5 +
           std::clamp(percent - 90, 0, 5) * 2.0 + std::clamp(percent - 95, 0, 5) * 3.0;
  }
  const Point &oldest() const { return points_[head_]; }
  const Point &newest() const { return points_[(head_ + count_ - 1) % CAPACITY]; }
  void append(Point p) {
    if (count_ == CAPACITY) {
      head_ = (head_ + 1) % CAPACITY;
      --count_;
    }
    points_[(head_ + count_) % CAPACITY] = p;
    ++count_;
  }
  void begin(const Sample &s, Mode mode) {
    started_ = true;
    mode_ = mode;
    head_ = count_ = 0;
    began_at_ = last_at_ = progress_at_ = s.at_ms;
    began_percent_ = best_ = last_raw_ = s.percent;
    append({s.at_ms, s.percent});
  }
};
static_assert(sizeof(Tracker) <= 1152, "Battery history must remain bounded");
} // namespace ya::battery_stats
