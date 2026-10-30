# 构建、刷写和恢复

## 固定环境

使用 ESP-IDF tag `v5.5.5`（commit `b774170ff46c393eeb5e495ea37936038d3f4f4f`）、项目 `sdkconfig.defaults`、`main/idf_component.yml` 和 `dependencies.lock`。工具链需匹配 IDF tools.json，不能复用 PATH 中不同版本的 Xtensa GCC。

初始化时将 IDF 克隆到 `.tools/esp-idf`，将 Python venv 建在 `.tools/venv`；设置 `IDF_TOOLS_PATH=$PWD/.tools/espressif` 后运行 IDF 的 `idf_tools.py install --targets esp32s3` 和 `install-python-env`。安装 Python requirements 后再用官方 v5.5 约束文件检查依赖。

```sh
./tools/idf.sh build
```

首次构建前以 `python -m espsecure generate_signing_key --version 2 private/keys/ota-signing.pem` 创建 RSA-3072 密钥，目录权限 0700，文件 0600。密钥丢失会导致现有设备不能接受后续签名 OTA，必须通过 USB 重新建立信任。不要每次构建重新生成密钥。

中文字体基于 Noto Sans SC，英文和数字使用 Inter 4.1，许可分别见 FONT-LICENSE.txt 和 INTER-LICENSE.txt。使用 lv_font_conv 1.5.3 和 `tools/fonts.py` 从 main、examples 的实现与头文件提取中文字符。字重/字号、2×单位、来源校验与再生方法见 [字体与排版](TYPOGRAPHY.md)。添加界面文字后重新生成；任意中文文件名/SSID 超出已包含字符集时需要扩充字体，不能据当前子集承诺完整 CJK 支持。

## 首次刷写

1. 确认端口对应 Espressif 303A:1001，读取 chip/flash/security 信息。
2. `esptool --port PORT read_flash 0 0x1000000 private/backups/original-flash.bin`，验证 16MB 长度和 SHA-256。
3. 核对生成配置：rollback、signed-on-update 开启；硬件 Secure Boot、Flash 加密、anti-rollback 和测试故障选项关闭。
4. 旧出厂分区布局与本项目不同。首次转换时明确初始化新 NVS/otadata，不能把出厂 NVS 当作本项目配置。首次刷写工具只在完整备份校验成功后执行。
5. 写入 bootloader、分区表、初始化数据和签名应用。后续日常更新只上传签名应用 .bin，不上传合并镜像。

USB flash 会短暂重启设备。普通 OTA 不擦除设置。新固件保存配网信息在自己的 NVS 命名空间，不编译进镜像。

## 恢复原固件

保持 BOOT 按键并重新连接 USB 进入下载模式，使用原始全 Flash 备份恢复：

```sh
python -m esptool --chip esp32s3 --port PORT write_flash 0 private/backups/original-flash.bin
```

恢复前核对备份哈希和目标设备。不要对其他已连接串口执行这些命令。原始镜像可能含 Wi-Fi 或服务凭据，不能公开分享。

## 开发诊断

启动内存策略：保持`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=1024`和`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768`。系统在配置/电源管理之后优先初始化NimBLE控制器，保护其专用保留内存；完成BSP、音频及首帧DMA后，在Wi-Fi/HTTP/数据服务创建之前将普通对象切换为PSRAM优先。不要在BSP/SPI/首帧准备之前全局设置阈值0。BLE主机任务/广播仍在数据服务创建后启动，显式内部任务栈和DMA缓冲不迁移到PSRAM。USB `status`增加`retention_heap/retention_largest/dma_heap/dma_largest`，分别观察专用能力的总空闲和最大连续块，不能用总PSRAM空闲量代替。

`tools/device.py` 通过 USB JSON 行协议读取状态和导出屏幕渲染图。例：`python tools/device.py '{"op":"status"}'`。Wi-Fi 配网用权限 0600 的 JSON 输入文件加 `--file`；不要将密码放进 shell 命令历史。此接口是物理 USB 开发接口，不提供远程网络 shell。

页面 ID：0 桌面、1 设置、2 系统状态、3 Wi-Fi、4 BLE、5 显示、6 声音、7 存储、8 OTA、9 系统、10 四角校准、12 日期与时间；9 为系统状态兼容入口。`{"op":"page","id":2}` 用于切换验收页面。`--snapshot artifacts/battery.png` 导出渲染图；截图不能证明实体屏幕的触摸或物理遮挡情况。

新增电源设置诊断页面ID15、日志应用诊断页面ID14，默认Warning，使用及构建缓存迁移见[LOGS.md](LOGS.md)。

电池统计开发分支在页面2显示耗电统计与充放电剩余时间，USB/BLE `status`包含`battery_stats`；算法、采样和重启/深睡重置边界见[BATTERY_STATS.md](BATTERY_STATS.md)。

可使用 `tools/bootstrap.sh` 完成项目内环境准备；已有 SDK 或签名密钥不会被覆盖。首次准备字体使用 `tools/bootstrap.sh --fonts`，仅安装字体工具、下载并校验固定来源、生成字体，不初始化SDK或创建签名密钥。之后可离线执行 `.tools/venv/bin/python tools/fonts.py`。三档中文为20/24px Regular及28px Medium，英文数字按对应尺寸原生栅格化。

首次转换分区的可复用入口为 `tools/first_flash.py --port PORT --backup BACKUP --backup-sha256 SHA256`。它拒绝校验失败的备份和 test 版本，校验签名后才刷写。该工具会重置应用配置，只用于首次安装；正常迭代必须使用 OTA。

发布构建建议使用 `tools/build_release.sh 0.2.0`（以后传入新的版本号），输出到build-release，明确关闭故障注入和临时HTTPS测试CA，避免旧CMake缓存沿用测试参数。`tools/release.py --image build-release/waveshare-ya.bin --url HTTPS_URL` 生成发布文件。交付现场使用build-patch目录构建0.1.1，其配置和实测指纹记录在验收报告中。

源码包保留第三方许可证，排除private、工具链、构建缓存和本地测试记录。`tools/export_artifacts.py` 使用显式目录白名单生成源码与首次安装包；`tools/validation_report.py` 只有在规定的稳定性记录完整时才生成最终报告。

## 0.2.0 回归诊断

`tools/regression_020.py --port PORT` 验证设置、页面生命周期、联网校时和软件滚动，恢复原偏好；运行前先核实实际设备。`{"op":"ui"}` 返回有限可见标签文本，跳过输入框和HTTP访问地址；用于比较LVGL状态与截图，不证明物理面板刷新。

`{"op":"scroll","y":1000}` / `{"op":"scroll","y":0}` 在当前页面执行软件滚动，`{"op":"metrics_reset"}` 请求下次刷新开始重置统计。status 的 actual_fps 来自适配器最终flush完成计数；refresh_p50_us/p95_us 为最近最多128个连续有绘制刷新间隔（排除>500ms空闲间隔），不是面板扫描频率；refresh_work_p95_us 和 flush_wait_p95_us 为LVGL刷新工作/等待耗时。long_gap_count/max_gap_us同时保留长间隔（可能是空闲，也可能是卡顿），连续帧p95不代表没有长卡顿。软件滚动不代替触摸采样和实体拖影测试。

`tools/export_artifacts.py --build build-release --out artifacts/release/0.2.0` 使用允许目录导出，保留历史版本。发布文件包含正式manifest.json；部署须先放固件再切换清单。

历史工具 patch_regression.py、finish_validation.py、validation_report.py 面向0.1.1历史记录，不能用于0.2.0交付或覆盖其验收报告。0.2.0使用regression_020.py、render_benchmark.py、negative_020.py、rtc_reboot_check.py，并按实际构建指纹记录结果。

## 0.3.0 省电配置构建

0.3.0 必须启用 PM 与 Tickless Idle。已有 0.2.0 的 sdkconfig 不会被 defaults 自动覆盖；请用 `./tools/build_release.sh 0.3.0`，或按 [Chrome 数据站说明](COMPANION.md) 在 build-data 使用独立 SDKCONFIG。旧配置缺少这些选项时 CMake 会明确拒绝构建，避免生成启动即失败的镜像。

## 0.4.0 Wi-Fi构建

`./tools/build_release.sh 0.4.0`同步已有构建配置中的K/V/R开关。新目录也可用 `./tools/idf.sh -B build-wifi -D SDKCONFIG=build-wifi/sdkconfig -DPROJECT_VER=0.4.0 -DYA_FAULT=0 -DYA_HTTPS_FIXTURE=OFF build`。现有sdkconfig不会自动接受新增defaults；缺少KVR时CMake明确拒绝。网络行为及诊断接口见[WIFI.md](WIFI.md)。

## 0.5.0 诊断补充

页面13为通知应用。USB `notifications`只读返回最近10条及未读数；`notifications_clear`提交异步清空，属于删除历史记录的动作。BLE链路分阶段状态、服务注册检查、数据路由和主动断开原因见USB status。发布构建仍使用固定工具链；本轮最终USB安装与OTA验证边界见VALIDATION.md。

## 0.5.1 可复现发布

`VERSION` 是默认版本来源；`./tools/build_release.sh` 无参数时使用它。正式 Tag 必须为 `v` 加 VERSION。保留依赖锁中的所有版本及哈希，只将本地 BSP 路径改成相对路径；CMake 在构建目录生成绝对路径副本，构建后 `tools/check_lock.py` 验证没有依赖漂移。CI固定 IDF commit 与组件管理器。

公开诊断工具不再默认选取某台设备：`tools/device.py --port PORT` 明确指定端口；历史工具使用 `YA_DEVICE_PORT`，在线测试另需 `YA_TEST_MANIFEST_URL`。这些硬件工具可能操作设备，CI 不运行它们。

GitHub 构建、签名与部署见 [RELEASE.md](RELEASE.md)。发布包内 `docs/ota-signing.pub` 仅是现有信任的公钥，可用于验证签名；私钥只存在本机 private/ 或 GitHub Actions Secret。
