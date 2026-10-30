#include "../main/battery_estimator.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>

using namespace ya::battery_stats;
static Sample sample(int minute, int percent, bool charging = false) {
  return {int64_t(minute) * 60000, percent, true, true, charging, charging, !charging, false};
}
static Summary discharge(Tracker &t, int first, int last, int start = 0, int duration = 10) {
  Summary out;
  for (int m = 0; m <= duration; ++m)
    out = t.update(sample(start + m, first - (first - last) * m / duration));
  return out;
}
int main() {
  // Real-time replay: 10 percentage points/hour, enough evidence before showing ETA.
  Tracker t;
  auto out = discharge(t, 80, 75, 0, 30);
  assert(out.phase == Phase::READY && out.remaining_minutes == 450);
  assert(std::abs(out.recent_rate - 10) < 0.001);
  assert(out.session_seconds == 1800 && out.change_percent == 5);

  // Sample duration AND SOC movement are required. No infinite/zero estimate on a plateau.
  Tracker warm;
  for (int m = 0; m < 5; ++m)
    assert(warm.update(sample(m, 80 - m)).phase == Phase::LEARNING);
  assert(warm.update(sample(5, 75)).phase == Phase::READY);
  Tracker flat;
  for (int m = 0; m < 180; ++m) {
    out = flat.update(sample(m, 50));
    assert(out.phase == Phase::LEARNING && out.remaining_minutes == -1);
  }
  Tracker tiny;
  out = discharge(tiny, 80, 79, 0, 60);
  assert(out.phase == Phase::LEARNING);

  // Follow recent use rather than retaining an old heavy-load rate forever; ring wraps.
  Tracker rolling;
  for (int m = 0; m <= 360; ++m)
    out = rolling.update(sample(m, m <= 120 ? 100 - m / 4 : 70 - (m - 120) / 12));
  assert(out.change_percent == 50 && out.session_seconds == 21600);
  assert(out.recent_seconds <= 7200 && std::abs(out.recent_rate - 5) < 0.01);
  assert(out.remaining_minutes == 600);

  // A one-point rebound doesn't count as additional consumption on return.
  Tracker noise;
  discharge(noise, 80, 75);
  out = noise.update(sample(11, 76));
  assert(out.phase == Phase::READY && out.change_percent == 5);
  out = noise.update(sample(12, 75));
  assert(out.change_percent == 5);
  out = noise.update(sample(13, 77)); // larger correction starts a new segment
  assert(out.phase == Phase::LEARNING && out.change_percent == 0);
  out = noise.update(sample(14, 70)); // sudden gauge jump is not load measurement
  assert(out.phase == Phase::LEARNING && out.change_percent == 0);

  // Missing/invalid telemetry, absent battery, bad SOC, time reversal and long gaps.
  for (int kind = 0; kind < 7; ++kind) {
    Tracker reset;
    discharge(reset, 80, 75);
    auto s = sample(11, 74);
    if (kind == 0)
      s.valid = false;
    if (kind == 1)
      s.present = false;
    if (kind == 2)
      s.percent = -1;
    if (kind == 3)
      s.percent = 101;
    if (kind == 4)
      s.at_ms = -1;
    if (kind == 5)
      s.at_ms = 0;
    if (kind == 6)
      s.at_ms += 120000;
    out = reset.update(s);
    assert(out.remaining_minutes == -1 && out.phase != Phase::READY);
    out = reset.update(sample(15, 73));
    assert(out.phase == Phase::LEARNING);
  }
  Tracker duplicate;
  discharge(duplicate, 80, 75);
  assert(duplicate.update(sample(10, 75)).phase == Phase::LEARNING);

  // USB pause/weak supply/contradictory status never extrapolates discharge or charge.
  Tracker supply;
  discharge(supply, 80, 75);
  auto usb = sample(11, 75);
  usb.usb = true;
  out = supply.update(usb);
  assert(out.mode == Mode::USB && out.remaining_minutes == -1);
  usb.discharging = false;
  assert(supply.update(usb).phase == Phase::UNAVAILABLE);
  usb.charging = usb.discharging = true;
  assert(supply.update(usb).remaining_minutes == -1);
  assert(supply.update(sample(12, 74)).phase == Phase::LEARNING);

  // Low-SOC charging includes an explicitly conservative, uncalibrated finishing allowance.
  Tracker charge;
  for (int m = 0; m <= 10; ++m)
    out = charge.update(sample(m, 50 + m, true));
  assert(out.phase == Phase::READY && std::abs(out.recent_rate - 60) < 0.001);
  assert(out.remaining_minutes == 60); // simple linear projection would incorrectly say 40
  assert(out.change_percent == 10 && out.mode == Mode::CHARGING);
  // Refits the same model as observed progress slows across upper SOC bands.
  Tracker taper;
  for (int m = 0; m <= 20; ++m)
    out = taper.update(sample(m, 85 + m / 2, true));
  assert(out.phase == Phase::READY && out.remaining_minutes > 10);
  for (int m = 21; m <= 36; ++m)
    out = taper.update(sample(m, 95 + (m - 20) / 4, true));
  assert(out.phase == Phase::READY && out.remaining_minutes >= 5);

  // No progress means unknown, not a stuck countdown or a fabricated charger fault.
  for (int m = 11; m <= 30; ++m)
    out = charge.update(sample(m, 60, true));
  assert(out.phase == Phase::NO_PROGRESS && out.remaining_minutes == -1);
  auto resumed = charge.update(sample(31, 61, true));
  assert(resumed.phase == Phase::READY && resumed.remaining_minutes > 0);
  auto paused = sample(32, 61, true);
  paused.charging = false;
  assert(charge.update(paused).remaining_minutes == -1);
  assert(charge.update(sample(33, 62, true)).phase == Phase::LEARNING);

  // 100% alone never implies completed charging; only PMU charge-done does.
  Tracker full;
  out = full.update(sample(0, 100, true));
  assert(out.phase == Phase::CONFIRMING_FULL && out.remaining_minutes == -1);
  auto done = sample(1, 100, true);
  done.full = true;
  assert(full.update(done).phase == Phase::CONFIRMING_FULL);
  done.charging = false;
  out = full.update(done);
  assert(out.phase == Phase::FULL && out.remaining_minutes == 0);
  done.usb = false;
  done.discharging = true;
  assert(full.update(done).phase == Phase::LEARNING); // stale PMU full flag after unplugging
  auto not_done = sample(2, 100, true);
  not_done.charging = false;
  assert(full.update(not_done).phase == Phase::UNAVAILABLE);

  // Near-empty battery, multi-day monotonic uptime and negative input do not overflow.
  Tracker empty;
  out = discharge(empty, 3, 0);
  assert(out.phase == Phase::READY && out.remaining_minutes == 5);
  Tracker long_uptime;
  out = discharge(long_uptime, 80, 75, 60 * 24 * 100, 30);
  assert(out.remaining_minutes == 450);
  puts("battery estimator replay and boundary scenarios passed");
}
