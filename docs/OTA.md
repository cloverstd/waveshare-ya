# OTA 发布、验证和回退

## 分区和信任

两个 6MiB 应用槽 `ota_0` / `ota_1`，无 factory 恢复应用。详细偏移见 partitions.csv。OTA 只写未运行槽，bootloader、分区表与其他数据分区不经 OTA 更新。所有首刷和 OTA 应用均带 RSA-PSS 签名；启用 ESP-IDF 软件签名验证，不烧 eFuse。

应用镜像自定义描述符包含 magic、板型和 `ab6-v1` 布局标识，位于标准 app descriptor 之后。上传时先检查标识和尺寸，再写入；完成后由 `esp_ota_end` 验证签名。检查失败不改变启动分区。

发布工具输入已经签名的应用镜像，会验证签名，生成 `.bin` 和 `manifest.json`。默认更新源为 `https://ya.cloverstd.com`，设备请求 `/api/v1/ota/manifest.json`；服务器实现所需路径、响应、字段限制和发布步骤见 [OTA 服务器接口文档](OTA_API.md)。自定义 HTTPS 清单地址仍可通过设置修改。

```sh
.tools/venv/bin/python tools/release.py --image build-release/waveshare-ya.bin
# 非默认托管位置可显式覆盖二进制 URL：
.tools/venv/bin/python tools/release.py --image build-release/waveshare-ya.bin --url https://your-host/path/waveshare-ya-0.2.0.bin
```

清单字段：`product`、`board`、`layout`、`version`、`url`、`size`、`sha256`、`notes`。HTTPS 校验证书，拒绝重定向；部署时提供直达 URL。清单必须是小于 4096 字节的扁平 JSON，字段不能重名，`size` 必须是整数；清单版本必须和签名应用内版本一致。下载失败不自动反复安装。设备有正确时间才能验证证书，联网后通过 SNTP 校时。

设备显示检查、传输、校验、准备重启和首次启动验证等阶段。进度条及字节数表示已接收并写入的固件数据，100% 仅代表传输完成；签名通过并不等于新版本已完成启动验证。只有连续三十秒健康检查通过，才显示“升级完成”。失败保留具体原因和最后传输位置，下一次尝试清零。下载进度事件最多每 100ms 发布一次，后台升级不因离开升级页面而停止。

升级页顶部只显示当前版本号与实际配置的清单URL，过长时仅URL横向滚动。检查发现新版本后，才显示并排的增量/全量选项和安装按钮；增量不可用时禁用该选项并说明原因。传输期间集中显示进度，下载量与重建写入量分别用B/KB/MB等单位表示（按1024换算），安装确认框沿用相同格式。100%后仍显示校验阶段，新槽启动验收使用独立状态提示。

底部一排为“二维码”“网页上传”和更新地址设置图标。“网页上传”开启十分钟会话，再点击“二维码”扫码；修改清单地址仍通过原有异步设置接口。页面只持有LVGL控件，离开时随父容器清理；OTA继续由后台系统服务处理。布局与硬件验收边界见VALIDATION.md。

局域网网页会话由屏幕手动打开，十分钟有效。凭证放在 URL fragment 中，页面移除地址栏凭证后通过 Authorization header 上传。页面不开放跨域访问。凭证不要写入日志、截图公开附件或分享链接。网页入口采用局域网 HTTP，固件真实性依赖镜像签名；不用于传输 Wi-Fi 密码。

## 新版确认

只有完整下载、签名通过、目标槽配置准备完成，才更新 otadata。新版本为 PENDING_VERIFY，检查显示/触摸/PMU/配置初始化及 UI/服务心跳 30 秒后调用 ESP-IDF mark-valid。验证失败、崩溃或复位会回退。未联网、无卡、无 BLE 外设均不是失败条件。

配置使用 `config_a` / `config_b` 两个命名空间。切换前把当前 v1 配置复制给目标槽；待验证的新版本只写自己的槽，旧版配置不会被新版本修改覆盖。后续迁移必须保留旧 schema 数据，验证成功前不得跨槽清理。不允许遇到 NVS 错误直接 erase-all。

纯电池启动升级要求有效且新鲜的电量读数 >=30%；USB 供电可升级。升级互斥，待验证状态禁止再次升级。设置变更和网络切换在升级期间拒绝。

已确认版本后续运行错误不保证自动回退；双槽同时损坏或 bootloader 损坏需要 USB。只有基于实际断电的测试才能声称断电验收；装电池时拔 USB 不等于断电。

## 开发故障测试

`CONFIG_YA_OTA_TEST_FAILURE=1` 在新版本待验证阶段 abort；`=2` 注册看门狗后停止喂狗。正常发布必须为 0。`CONFIG_YA_TEST_HTTPS_CA` 仅用于本地 HTTPS 测试夹具，正式构建必须关闭。不得把故障镜像作为正常发布包。

应用主任务从初始化开始受看门狗保护。待验证阶段只有健康检查通过才喂狗，连续健康30秒且BLE服务同步完成后确认版本；即使回退API无法找到可用旧槽，也不会把失败版本标记为有效。

清单读取有30秒总时限；固件传输有10分钟总时限，外加有界错误响应清理时间。慢速但持续发送数据也不会永久占用升级状态。

本项目提供 CMake 测试开关，优先于 Kconfig 默认值：`-DYA_FAULT=1 -DPROJECT_VER=0.1.0-test-abort` 或 `-DYA_FAULT=2 -DPROJECT_VER=0.1.0-test-watchdog`。正常发布使用 `-DYA_FAULT=0 -DYA_HTTPS_FIXTURE=OFF`。测试CA通过 `-DYA_HTTPS_FIXTURE=ON` 启用，仅用于 `tools/https_fixture.py` 生成的私有夹具。发布工具默认拒绝 test 版本，测试包装需显式 `--allow-test`。

在线托管的清单与固件均需 HTTP 200、精确 `Content-Length`；清单 `Content-Type` 为 `application/json`（可附 `charset=utf-8`），固件为 `application/octet-stream`。禁止分块传输、代理压缩或转码；不跟随 HTTP 重定向。发布工具默认输出正式域名下的版本化二进制地址，本地发布产物不会自动上传服务器；正式 Tag 由 GitHub Actions 构建并发布 Release，Workers 自动发现最新正式版本，见 RELEASE.md。

局域网 `GET /api/status` 保留 `version/result/progress/pending`，新增 `phase/received/total`；`phase` 是稳定英文状态，`received/total` 单位为字节。该接口与 `POST /api/ota` 都由设备提供，不属于公网更新服务器需要实现的接口。相关鉴权仍是十分钟会话的 Bearer token。

## 0.6.0 增量更新

进入设备“固件升级”页面自动提交一次后台检查，保留“重新检查”。离线显示原因，不循环请求；已有检查合并，升级/启动验证中恢复显示进度。正式版本按数字语义版本比较，只对较新版本开放在线安装；局域网页上传仍接受完整签名应用 `.bin`。

页面提供全量、增量选项及各自下载大小；匹配且更小的补丁默认选中。增量不可用时显示原因并禁用该选项，用户仍可全量安装。确认框绑定目标SHA和方式，更新源变化使旧检查失效。增量失败保留错误与原固件，由用户重试或改选全量，不自动下载全量。

增量使用esp_delta_ota 1.1.4与detools sequential/heatshrink原始补丁流（不是示例工具附加64字节头部的格式）。从当前槽读取旧签名镜像，流式重建到备用槽；旧镜像完整SHA256/长度、补丁SHA256、目标完整SHA256/长度与签名依次核对，完整镜像包含原始签名尾部。不要用app_elf_sha256或esp_partition_get_sha256替代完整文件摘要。无额外补丁分区、无需TF卡。基线哈希在后台计算；Flash任务保持内部RAM栈和有界读写，失败释放解码器及连接。

进度条按所选下载包大小计算，`written`另记重建后的应用字节；下载100%后还要完成解码、签名校验和30秒启动验证。沿用ab6-v1、配置槽复制与回退机制。旧固件首次获得该能力需先通过全量OTA安装0.6.0或以后版本。

本地生成（先通过release.py或export_artifacts.py准备全量发布目录）：

```sh
.tools/venv/bin/python tools/delta_release.py artifacts/release/0.6.0 \
  --base artifacts/release/0.5.1/waveshare-ya-0.5.1.bin
# CI从公开Release下载最近三个较低正式版本的原始镜像：
.tools/venv/bin/python tools/delta_release.py artifacts/publish --github-bases
```

生成器固定detools 0.53.0 / heatshrink2 0.15.0，验证基线及目标的既有生产签名，生成后逐字节还原目标，再将索引指针加入全量清单。只有更小的补丁进入索引。没有历史版本时允许空索引；历史镜像验证或还原错误导致发布失败。线上清单和补丁均由Tag CI生成，不可用本地重新编译镜像替代同版本CI基线。

物理USB诊断增加`install`可选字段`mode`（`full`默认或`delta`）及`sha256`（目标完整摘要；省略则绑定提交时已检查的目标）。`status`包含`ota_phase/ota_mode/ota_received/ota_total/ota_written/full_size/delta_size/delta_available/target_sha256`，可用于实机验收。
