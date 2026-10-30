#pragma once
#include "log_buffer.hpp"
#include "esp_err.h"
namespace ya {
void logs_init();
void logs_load_level(); // after config_init, before board/services
int logs_level();
esp_err_t logs_setting(int level); // non-GUI: persist, then apply
bool logs_snapshot(LogBuffer &out, uint32_t &dropped); // try-lock, no I/O
bool logs_clear();
} // namespace ya
