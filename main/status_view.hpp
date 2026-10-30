#pragma once
#include "lvgl.h"
#include "battery_estimator.hpp"
#include <cstdint>
namespace ya {
struct StatusView {
  lv_obj_t *battery, *percent, *source, *charge;
  lv_obj_t *usage, *change, *direction, *eta, *session, *rate;
  lv_obj_t *resources[4], *values[4], *notes[4], *links[2];
};
struct StatusViewData {
  bool valid, present, usb, charging;
  int percent;
  battery_stats::Summary stats;
  uint64_t total[4], free[4];
  bool available[4];
  const char *wifi, *bluetooth;
};
StatusView status_view_create(lv_obj_t *parent);
void status_view_update(StatusView &view, const StatusViewData &data);
} // namespace ya
