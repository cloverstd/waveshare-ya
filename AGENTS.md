# Working on Waveshare YA

Read README.md and the relevant guide in docs/ before changing platform behavior. The target is ESP32-S3-Touch-AMOLED-2.16; other Waveshare models are not interchangeable.

- Use the pinned ESP-IDF/BSP/LVGL versions and dependency lock. The local BSP adaptation is documented in its YA_PATCH.md.
- Add apps through the registry and lifecycle interface. Reuse system services; do not initialize a second I2C, display, audio or network stack.
- Keep network/storage work off the GUI thread. Clean up application timers, subscriptions and tasks on exit. Do not hold a GUI lock while waiting for I/O.
- Keep all UI, popups and keyboards within the shared rounded safe area. Update generated font assets when adding text; retain font licenses.
- Preserve the A/B partition contract and configuration rollback behavior. OTA packages are signed application images, not merged flash images. Normal changes do not write eFuses.
- Before USB writes, identify the actual device and verify the original backup. Honor the user's existing authorization; do not ask again for already authorized work.
- Never commit private/, credentials, signing keys, backups, build outputs or toolchains. Export artifacts using an explicit allowlist.
- Match verification to the change. Distinguish compile checks, device tests, user-observed physical checks and untested hardware conditions in docs/VALIDATION.md. Do not call a running soak test passed.
