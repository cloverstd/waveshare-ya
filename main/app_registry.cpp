#include "ya.hpp"
namespace ya {
extern const App data_app;
const App apps[] = {
    {"settings", "设置", LV_SYMBOL_SETTINGS, 0, ui_settings, nullptr, nullptr, nullptr},
    {"status", "系统状态", LV_SYMBOL_LIST, 1, ui_battery, nullptr, nullptr, nullptr},
    data_app,
    {"notifications", "通知", LV_SYMBOL_BELL, 0, ui_notifications, nullptr, nullptr, nullptr},
    {"logs", "日志", LV_SYMBOL_FILE, 0, ui_logs, nullptr, nullptr, ui_logs_destroy},
};
const size_t app_count = sizeof(apps) / sizeof(apps[0]);
} // namespace ya
