# Waveshare YA 基础固件

ESP32-S3-Touch-AMOLED-2.16 专用，ESP-IDF 5.5.5 / 官方 BSP 2.0.1 / LVGL 9.3.0。

当前源码版本 **0.6.0**：固件升级页面进入时自动检查新版本，可选择全量或增量更新；有匹配补丁时默认增量，失败后可手动重试或选择全量。发布链路为最近三个正式版本生成补丁。实机与线上发布状态见验收记录。

中文深色应用桌面、Wi-Fi/BLE 设置、亮度/音量/目标帧率、分供电息屏、系统状态、北京时间与RTC保时，以及网页上传和 HTTPS 在线签名 OTA。应用按编译期注册，后续相册和 Home Assistant 可复用同一服务层。

- [构建、首次刷写与恢复](docs/BUILD.md)
- [网站部署、CI 与首次发布](docs/RELEASE.md)
- [OTA 发布与回退](docs/OTA.md)
- [OTA 服务端接口合同](docs/OTA_API.md)
- [应用开发与系统服务](docs/APPS.md)
- [日志应用与记录级别（默认 Warning）](docs/LOGS.md)
- [Wi-Fi、多网络与 KVR](docs/WIFI.md)
- [BLE 协议](docs/BLE.md)
- [Chrome 数据站与 0.3.0 使用说明](docs/COMPANION.md)
- [省电与分阶段休眠设计（0.3.0 源码已实现，硬件待验收）](docs/POWER_MANAGEMENT.md)
- [统一数据协议与多网关设计（0.3.0 源码已实现，硬件待验收）](docs/DATA_PROTOCOL.md)
- [验收记录与未验证项目](docs/VALIDATION.md)

`private/` 包含备份、Wi-Fi 测试输入和签名私钥，绝不提交或打入发布包。`artifacts/` 存放构建产物与本地截图。新应用需重新编译整个固件，不支持动态安装任意程序。

设备的蓝牙是 BLE，不支持传统蓝牙音箱/耳机。目标帧率是 UI 调度上限，不等同于面板扫描频率或保证达到的实际帧率。没有 TF 卡不会妨碍启动或 OTA。

## 交付文件

正式固件由 GitHub Actions 构建并发布到 [GitHub Releases](https://github.com/cloverstd/waveshare-ya/releases)，网站为 [ya.cloverstd.com](https://ya.cloverstd.com/)，蓝牙数据站位于 [/companion/](https://ya.cloverstd.com/companion/)。Release，包含签名 OTA 应用、正式清单、首次安装包、源码包和 SHA256SUMS。日常升级使用设备“设置 → 固件升级”的在线更新或网页上传，只安装签名应用 `.bin`。默认清单地址为 `https://ya.cloverstd.com/api/v1/ota/manifest.json`，服务器接口见 OTA_API 文档。

正视屏幕且 USB 在下：左键返回，右键主页，中键短按息屏/唤醒、长按2秒有序关机。关机后按中键开机；持续长按6秒为 PMU 硬件紧急关机，不能依赖软件升级保护拦截。电池息屏默认30秒、USB默认5分钟，分别可配置。

验收报告按构建分别记录结果。历史版本的测试不代表 0.6.0 已通过；实体屏拖影、按键和电源测试以新版本实际记录为准。
