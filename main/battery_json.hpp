#pragma once
#include "battery_estimator.hpp"
#include "cJSON.h"

namespace ya {
inline void add_battery_stats(cJSON *out, const battery_stats::Summary &s) {
  auto *j = cJSON_AddObjectToObject(out, "battery_stats");
  if (!j)
    return;
  cJSON_AddStringToObject(j, "mode", battery_stats::name(s.mode));
  cJSON_AddStringToObject(j, "phase", battery_stats::name(s.phase));
  cJSON_AddNumberToObject(j, "session_seconds", s.session_seconds);
  cJSON_AddNumberToObject(j, "change_percent", s.change_percent);
  cJSON_AddNumberToObject(j, "recent_seconds", s.recent_seconds);
  if (s.session_rate >= 0)
    cJSON_AddNumberToObject(j, "session_percent_per_hour", s.session_rate);
  else
    cJSON_AddNullToObject(j, "session_percent_per_hour");
  if (s.recent_rate >= 0)
    cJSON_AddNumberToObject(j, "recent_percent_per_hour", s.recent_rate);
  else
    cJSON_AddNullToObject(j, "recent_percent_per_hour");
  if (s.remaining_minutes >= 0)
    cJSON_AddNumberToObject(j, "remaining_minutes", s.remaining_minutes);
  else
    cJSON_AddNullToObject(j, "remaining_minutes");
}
} // namespace ya
