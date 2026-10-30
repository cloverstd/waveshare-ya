# Local BSP adaptation

Based on waveshare/esp32_s3_touch_amoled_2_16 2.0.1, registry source commit bfeb6e6d5737178cdde78b630c8118074da0a657, Apache-2.0.

Use two **12-row internal** RGB565 LVGL draw buffers in 0.2.0 (480 × 12 × 2 bytes each, 23,040 bytes total). Keep `use_psram=false` and `require_double_buffer=true`.

The original two 50-row PSRAM buffers caused ESP32-S3 SPI to allocate about 48KB of DMA bounce buffers at runtime. Wi-Fi/BLE coexistence made that allocation fail on the actual board. 0.1.1 used two 8-row internal buffers to avoid those allocations. 0.2.0 compares that baseline with 12 rows: the latter adds 7,680 bytes of predictable internal allocation and reduces repeated small-strip drawing work. Performance and concurrent OTA acceptance are recorded by build in `docs/VALIDATION.md`.

The buffer adaptation leaves pin mapping, 40MHz QSPI clock, panel initialization, touch and SDMMC drivers unchanged; the subsequent startup adaptation is described below. This does not add TE synchronization or claim a 60Hz full-screen rendering rate. Do not restore PSRAM draw buffers or increase their size without testing DMA and internal-heap availability.

0.3.0 adds `bsp_display_sleep_set(bool)` for the system power coordinator to turn
panel output off/on only after the LVGL adapter drains and pauses flush work.
Panel initialization starts with output off; board initialization applies zero
brightness for a timer wake and enables output only for an interactive boot.
This does not change pins, QSPI rate, buffers, or shared PMU rail configuration.
Electrical current and physical wake behavior still require board acceptance.

2026-10-30 startup display adaptation: the CO5300 initialization table now sets
brightness (`0x51`) to zero and ends with display-off (`0x28`), replacing the
previous full-brightness/display-on (`0x29`) interval. Brightness initialization
also stays at zero. Reset, initialization, output-off and brightness command
errors propagate to the caller instead of reporting successful initialization.
All other vendor commands and the 600 ms sleep-out delay are retained.

After the adapter starts, an interactive boot renders a full black startup view
before enabling output. The coordinator pauses the adapter outside the GUI lock;
the pinned SPI parameter-command path drains queued pixel DMA before applying
brightness and display-on. The spinner runs while the remaining board and system
services initialize, then is deleted with its root when the desktop is ready.
Timer-driven silent boots keep output off and create no spinner. Pin mapping,
QSPI speed, DMA buffers, PMU rails and dependency versions are unchanged. This
does not control the panel during ROM/bootloader execution; physical startup and
deep-sleep wake behavior require acceptance on the actual board.

2026-10-30 boot-memory fix: `bsp_display_lock()` converts the adapter's
`esp_err_t` result to its documented boolean contract with `== ESP_OK`.
Returning `ESP_OK` directly previously reported a successfully acquired lock
as false, causing the startup overlay to abort with a false timeout.

The system now initializes the NimBLE controller before board allocations to
protect its retention-memory allocation. The host task and advertising still
start only after board/data services exist. After BSP/audio initialization and
a drained first display flush (also on a silent boot), ordinary allocations
switch to PSRAM preference before Wi-Fi/HTTP/data initialization. Keep the
startup 1024-byte internal preference and the 32 KiB dedicated internal pool;
do not globally set the threshold to zero before SPI contexts/first flush are
ready. Both 12-row DMA buffers and all task stacks remain internal.
