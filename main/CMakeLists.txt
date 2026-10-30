idf_component_register(SRCS "logs.cpp" "notifications.cpp" "main.cpp" "data.cpp" "power.cpp" "data_app.cpp" "diagnostics.cpp" "core.cpp" "board.cpp" "network.cpp" "rtc.cpp" "metrics.cpp" "bluetooth.cpp" "ota.cpp" "ota_view.cpp" "ui.cpp" "app_registry.cpp" "font_zh.c"
    INCLUDE_DIRS "." "../components/XPowersLib/src"
    PRIV_REQUIRES wpa_supplicant
    REQUIRES espressif__esp_delta_ota nvs_flash app_update esp_http_server esp_http_client esp_https_ota esp_wifi esp_netif esp_timer esp_event esp_driver_gpio esp_driver_usb_serial_jtag bt json mbedtls esp_system fatfs spiffs)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)

target_link_libraries(${COMPONENT_LIB} INTERFACE "-u ya_firmware_identity")

if(DEFINED YA_FAULT)
  if(YA_FAULT GREATER 0 AND NOT PROJECT_VER MATCHES "test")
    message(FATAL_ERROR "Fault firmware must use a test version")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE YA_TEST_FAILURE=${YA_FAULT})
endif()
if(YA_HTTPS_FIXTURE)
  if(NOT PROJECT_VER MATCHES "test")
    message(FATAL_ERROR "Temporary HTTPS CA requires an explicit test version")
  endif()
  target_compile_definitions(${COMPONENT_LIB} PRIVATE YA_TEST_CA_ENABLE=1)
endif()
target_sources(${COMPONENT_LIB} PRIVATE "../examples/app_template/template.cpp")

target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)

# Profiling showed CPU rendering dominates scrolling. Optimize the pinned LVGL
# component for speed without changing the system/driver optimization or RAM buffers.
idf_component_get_property(ya_lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_compile_options(${ya_lvgl_lib} PRIVATE -O2)

if(NOT CONFIG_PM_ENABLE OR NOT CONFIG_FREERTOS_USE_TICKLESS_IDLE)
  message(FATAL_ERROR "YA 0.3 requires PM and Tickless Idle. Use tools/build_release.sh 0.3.0 or a fresh SDKCONFIG; see docs/COMPANION.md")
endif()

if(NOT CONFIG_ESP_WIFI_11KV_SUPPORT OR NOT CONFIG_ESP_WIFI_RRM_SUPPORT OR NOT CONFIG_ESP_WIFI_WNM_SUPPORT OR NOT CONFIG_ESP_WIFI_11R_SUPPORT)
  message(FATAL_ERROR "YA 0.4 requires KVR. Use tools/build_release.sh 0.4.0 or a fresh SDKCONFIG with sdkconfig.defaults; see docs/WIFI.md")
endif()
