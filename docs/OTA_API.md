# YA OTA 服务器接口文档

适用于 ESP32-S3-Touch-AMOLED-2.16、产品 `waveshare-ya`、分区布局 `ab6-v1`。默认 base URL 为 **`https://ya.cloverstd.com`**。服务器只需提供下列两个只读 GET，静态文件服务器也能实现；不需要设备注册、登录、上传、进度上报或推送接口。

## 1. 获取正式发布清单

```http
GET /api/v1/ota/manifest.json HTTP/1.1
Host: ya.cloverstd.com
Accept-Encoding: identity
```

无请求体、查询参数或鉴权。完整地址为 `https://ya.cloverstd.com/api/v1/ota/manifest.json`。

成功响应：

```http
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
Content-Length: <响应正文实际 UTF-8 字节数>
Cache-Control: no-store
```

正文直接返回对象，不包装在 `data` 中；字段区分大小写。以下对应本次 0.2.0 交付镜像；后续构建必须使用发布工具重新生成 `size` 和 `sha256`，不可沿用旧值：

```json
{
  "product": "waveshare-ya",
  "board": "esp32-s3-touch-amoled-2.16",
  "layout": "ab6-v1",
  "version": "0.2.0",
  "url": "https://ya.cloverstd.com/firmware/waveshare-ya/0.2.0/waveshare-ya-0.2.0.bin",
  "size": 2035712,
  "sha256": "3c12e5c047fea02dd1d78ff39b3e97464c51a382fa5cb6f7e6b410f5f8691c04",
  "notes": "显示、按键、电源管理、时间同步和 OTA 进度改进"
}
```

| 字段 | 类型 | 必需 | 约束 |
|---|---|---|---|
| `product` | string | 是 | 固定 `waveshare-ya` |
| `board` | string | 是 | 固定 `esp32-s3-touch-amoled-2.16`，不与其他 Waveshare 板型混用 |
| `layout` | string | 是 | 固定 `ab6-v1` |
| `version` | string | 是 | 1～31 个 ASCII 字节；首字符为字母/数字，其余允许字母、数字、`.`、`-`、`_`、`+`；须与签名应用描述符版本一致 |
| `url` | string | 是 | 直达完整 HTTPS 下载 URL，最多 383 个 ASCII 字节；不带账号密码、空白或 `#fragment`；非 ASCII 路径应百分号编码 |
| `size` | integer | 是 | 完整签名 `.bin` 字节数，512～6291456；不能是字符串或小数 |
| `sha256` | string | 是 | 对完整签名 `.bin` 计算的 SHA-256，64 个小写十六进制字符 |
| `notes` | string | 否 | 更新说明，最多 255 个 UTF-8 字节；省略时为空 |

整个正文必须为 1～4095 字节 UTF-8 JSON。禁止嵌套对象/数组、重复字段和原始或转义 NUL。额外的平面字段会忽略，但不建议添加。`notes` 字节数不同于汉字数。

设备比较清单版本与当前版本：相同显示“已是当前版本”；不同显示可用版本，用户确认后安装。这里不进行语义版本大小排序，也不自动降级或升级。服务器应始终提供希望用户安装的正式版本，**没有更新时仍返回同一份 200 清单**，不要返回 204 或 304。

错误响应可使用扁平 JSON：

```http
HTTP/1.1 404 Not Found
Content-Type: application/json; charset=utf-8
Cache-Control: no-store
```

```json
{"error":"not_published","message":"暂无已发布固件"}
```

尚未发布使用 `404`；临时故障使用 `503`，如 `{"error":"temporarily_unavailable","message":"请稍后重试"}`。其他非 200 状态也作为失败处理。固件依据 HTTP 状态显示本地提示，不依赖服务器错误正文，不自动重试安装。

## 2. 下载签名应用镜像

默认路径模板：

```text
/firmware/waveshare-ya/{version}/waveshare-ya-{version}.bin
```

例如：

```http
GET /firmware/waveshare-ya/0.2.0/waveshare-ya-0.2.0.bin HTTP/1.1
Host: ya.cloverstd.com
Accept-Encoding: identity
```

```http
HTTP/1.1 200 OK
Content-Type: application/octet-stream
Content-Length: <必须与 manifest.size 完全相同>
Cache-Control: public, max-age=31536000, immutable

<原始签名 .bin 二进制字节>
```

设备实际请求清单 `url`，因此也可以使用其他直达 HTTPS URL/CDN；发布工具的 `--url` 可覆盖默认路径。默认服务器不需要鉴权。下载中不使用 Range 或断点续传，失败后用户重试会从头下载。没有文件返回 `404`，暂不可用返回 `503`。

只提供**签名应用镜像**。不要提供 ZIP、源码包、合并 Flash 镜像、bootloader 或分区表。镜像包含设备既有信任密钥的 RSA-PSS 签名，服务器不需要私钥。不要重新签名、去除签名尾部、重新打包、压缩或改变任何字节。

## 3. HTTP/TLS 与发布要求

- 清单和二进制都必须直达 HTTP 200，并带准确 `Content-Length`。固件拒绝 `Transfer-Encoding: chunked`、压缩 `Content-Encoding`、错误媒体类型及 3xx 重定向；允许省略 `Content-Encoding` 或使用 `identity`。
- 证书链和域名须通过 ESP-IDF 公共 CA bundle 校验。正式固件不信任开发夹具 CA，不关闭证书或主机名校验。设备必须先有正确时间，Wi-Fi 联网后会校时。
- 清单读取总时限 30 秒，二进制传输总时限 10 分钟；单次网络操作还受 10 秒 socket 超时约束。因此超时关闭可能比总时限晚一个正在进行的网络操作，不应把时限当成严格毫秒 SLA。
- 先上传版本化二进制并核对实际响应，再原子替换清单。不要先发布清单指向不存在的文件。已发布的版本 URL 不变更内容；新构建使用新版本。
- 清单禁用陈旧缓存，二进制可以长期缓存。禁用代理自动压缩、鉴权登录跳转、下载页包装和 HTTP→HTTPS 跳转链；清单自身直接填写最终 HTTPS 地址。

本地打包命令（不要将签名私钥加入 Git 或发布附件）：

```sh
./tools/build_release.sh 0.2.0
.tools/venv/bin/python tools/release.py \
  --image build-release/waveshare-ya.bin \
  --out artifacts/release/0.2.0 \
  --notes '显示、按键、电源管理、时间同步和 OTA 进度改进'
```

上述命令产物为 `artifacts/release/0.2.0/waveshare-ya-0.2.0.bin` 和同目录 `manifest.json`。发布工具验证镜像签名，读取镜像内版本、计算完整大小和摘要，不需要手工填写这些字段。测试故障版本默认禁止发布。

将二进制部署到 `/firmware/waveshare-ya/0.2.0/waveshare-ya-0.2.0.bin`，将 `manifest.json` 内容部署到 `/api/v1/ota/manifest.json`。本地工具不会自动部署网站。可使用以下请求核对真实 GET 响应；不要加 `-L`，否则会掩盖固件不支持的重定向：

```sh
curl --fail --show-error --silent -D /tmp/ya-manifest-headers.txt \
  -o /tmp/ya-manifest.json https://ya.cloverstd.com/api/v1/ota/manifest.json
curl --fail --show-error --silent -D /tmp/ya-firmware-headers.txt \
  -o /tmp/ya-firmware.bin \
  https://ya.cloverstd.com/firmware/waveshare-ya/0.2.0/waveshare-ya-0.2.0.bin
shasum -a 256 /tmp/ya-firmware.bin
wc -c /tmp/ya-firmware.bin
```

## 4. 设备进度与安装结果

设备通过后台任务处理网络与 Flash，屏幕进度条不需要服务器推送。`received/total` 是已接收并写入/预期完整镜像字节数；`progress` 是传输百分比。达到 100% 后仍须校验 SHA-256、签名、完整镜像长度、板型与版本，并准备目标槽配置。

| `phase` | 屏幕含义 |
|---|---|
| `idle` | 尚未开始 |
| `checking` | 获取清单 |
| `available` / `current` | 有不同版本 / 当前已是清单版本 |
| `transferring` | 连接、接收并写入，进度 0～100% |
| `verifying` | 传输结束，校验签名与准备配置；100% 不代表升级完成 |
| `rebooting` | 已切换下次启动槽，即将重启 |
| `boot_verify` | 新版本启动后的 30 秒健康检查 |
| `complete` | 启动检查通过，新版本确认成功 |
| `failed` | 检查、传输、校验或配置准备失败 |
| `rolled_back` | 新版本启动未通过，已回退 |

每次新的检查或传输清空旧进度。离开升级页面不会取消已开始的升级。失败保留原因，下一次尝试从头开始。电池供电要求有效且新鲜的电量至少 30%；USB 供电允许升级。升级中的软件重启/关机和息屏由系统策略保护，无法保证用户物理断开所有电源不会中断流程。

局域网备用上传的接口属于**设备自身**：`POST http://设备IP/api/ota` 接收签名原始二进制；`GET http://设备IP/api/status` 返回设备阶段。二者使用从屏幕开启的十分钟 Bearer 会话，**无需在 ya.cloverstd.com 实现**。状态响应示例：

```json
{"version":"0.2.0","result":"正在接收并写入固件","progress":42,"phase":"transferring","received":840000,"total":2000000,"pending":false}
```

完整下载与验证通过前不改变启动槽。重启后新固件必须通过 30 秒健康检查才确认；失败回退的旧固件继续使用其原配置。公网服务器返回 200 或设备传输显示 100% 都不能作为升级成功凭据，最终以设备 `complete` 和运行版本为准。

## 5. 联调验收

服务端上线后检查：正常下载、同版本、404/503、证书错误、重定向、错误媒体类型/长度、压缩/分块响应、超时和中途断网。验证不合法清单（重复字段、嵌套、缺字段、分数大小、错误产品/板型/布局）均不启动安装。

设备还需验证摘要错误、签名错误、清单与镜像版本不同、截断镜像不切换槽；成功安装后有 30 秒 `boot_verify`，故障版本自动回退。编译和主机单元测试不能代替这些设备联调。真正断电验收必须切断 USB 和电池，具体已验证范围记录在 [VALIDATION.md](VALIDATION.md)。

## Workers 与 GitHub Release

正式服务实现于仓库 `worker/`。Tag `vX.Y.Z` 与固件内 `X.Y.Z`、清单版本一致。Worker 从公开仓库最新正式 Release 的 `manifest.json` 取得清单，内部最多缓存60秒；客户端仍收到 `no-store`。版本化下载从对应 Tag 的附件代理，GitHub 跳转只在 Worker 内部处理。固件经固定长度流输出，保持原字节和准确长度。未发布返回404，上游故障返回503；不缓存错误，也不提供任意 URL 代理。

站点首页及 `/companion/` 同属该 Worker。部署和 Secret 配置见 [RELEASE.md](RELEASE.md)。GitHub Actions 正式构建使用既有签名私钥的加密 Secret；服务器运行时不持有签名私钥。
