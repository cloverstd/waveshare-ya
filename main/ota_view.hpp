#pragma once
#include "lvgl.h"
#include <cstdint>

namespace ya {
struct OtaViewActions {
  lv_event_cb_t check, install, delta, full, qr, upload, settings;
};
struct OtaViewData {
  const char *source, *phase, *note, *version, *delta_note;
  bool available, busy, checking, active, pending, failed, delta_available, delta_selected;
  uint32_t received, total, written, full_size, delta_size;
  int progress;
};
// Presentational widgets only. Commands and OTA state remain owned by system services.
struct OtaView {
  lv_obj_t *root = nullptr, *source, *hero, *phase, *version, *note;
  lv_obj_t *transfer, *bar, *bytes, *written, *percent;
  lv_obj_t *choices, *delta, *full, *delta_size, *full_size, *hint;
  lv_obj_t *check, *install, *qr, *upload, *settings;
};
OtaView ota_view_create(lv_obj_t *parent, const char *version, const OtaViewActions &actions);
void ota_view_update(OtaView &view, const OtaViewData &data);
} // namespace ya
