# 网站与自动发布

公开仓库：<https://github.com/cloverstd/waveshare-ya>。正式网站：<https://ya.cloverstd.com/>；蓝牙数据站：<https://ya.cloverstd.com/companion/>。

## 一次性配置

仓库 Settings → Secrets and variables → Actions → Repository secrets：

| Secret | 用途 |
|---|---|
| `OTA_SIGNING_KEY_PEM` | 现有 RSA-3072 OTA 私钥的完整 PEM，只供 Tag 发布构建使用 |
| `CLOUDFLARE_API_TOKEN` | Workers 部署凭据 |
| `CLOUDFLARE_ACCOUNT_ID` | 托管 cloverstd.com 的 Cloudflare 账户 |

私钥不要放进 Git、Issue、聊天、日志或附件。初始化现有签名 Secret 可在本机运行 `gh secret set OTA_SIGNING_KEY_PEM --repo cloverstd/waveshare-ya < private/keys/ota-signing.pem`。CI 会与 `docs/ota-signing.pub` 比对，拒绝替换密钥。普通 main/PR 构建用临时测试密钥，不能作为现有设备的升级文件。

Cloudflare Token 使用官方 Edit Cloudflare Workers 模板，仅授权目标账户与 `cloverstd.com` zone。新权限模型下首次创建 Worker 需要该账户 Workers 产品 Admin，修改 Custom Domain 需要该 zone 的 Workers Routes Write；旧权限模型对应 Workers Scripts Edit、Workers Routes Edit 及账户/zone读取权限。已有站点绑定先核对，避免覆盖其他服务。配置将创建 `waveshare-ya-update` Worker 并绑定 `ya.cloverstd.com`；不使用 Pages、数据库、KV 或 R2。

参考：[Cloudflare Actions 配置](https://developers.cloudflare.com/workers/ci-cd/external-cicd/github-actions/)、[Workers 权限](https://developers.cloudflare.com/workers/authorization/workers/)。

GitHub 仓库启用 Settings → Releases → Enable release immutability。已发布的版本、附件和 Tag 不再修改。所有 Actions 固定 commit，npm 使用锁文件。

## 发版

1. 修改根目录 `VERSION` 为新的稳定版本 `X.Y.Z`，提交并推送 main，等待检查通过。
2. 创建并推送对应 Tag，例如 `git tag -a v0.5.1 -m 'YA 0.5.1'`、`git push origin v0.5.1`。Tag提交必须属于main；版本最长31字节。
3. `Check and release firmware` 从 Tag 构建签名应用，检查板型、布局、版本、6MiB限制、签名、依赖锁、测试与公开内容。正式构建禁止测试CA及故障注入，不自动生成替代生产密钥。
4. 仅当网页与固件检查都通过，先创建Draft Release，上传所有附件，重新下载并校验SHA256SUMS，再发布为Latest。重跑只允许更新尚未发布的Draft，已发布文件不得覆盖。
5. Workers最多60秒后发现新正式清单，下载通过版本路径代理对应Release附件。设备仍须用户确认安装。发布不会自动升级任何设备。

附件包括签名 `waveshare-ya-X.Y.Z.bin`、`manifest.json`、`SHA256SUMS`、首次安装ZIP、脱敏源码ZIP及`release-info.json`。后者记录源commit、Actions run和固件SHA256。首次安装ZIP不能通过OTA上传。

## 网站部署与本地开发

main中`worker/`、`web/`或部署workflow变化触发`Deploy update website`，检查通过后运行Wrangler，原子部署Worker与静态资源。Secret未配置时任务明确失败，不声称上线。配置好Secret后可从Actions手动运行该workflow。

```sh
cd worker
npm ci
npm run check
npm test
npm run dev
```

首页在`/`，现有web资源复制到`/companion/`。统一数据网关仍由用户自行提供，Worker不保存天气位置、手工数据或用户访问令牌。

Worker仅代理固定公开仓库，限制上游HTTPS主机和重定向次数；检查清单字段与大小，下载保留完整签名应用字节。对客户端清单禁缓存；Worker内部最多缓存60秒，版本化文件长期缓存。错误不缓存。下载使用FixedLengthStream，不能改成普通ReadableStream输出，否则可能失去Content-Length。

线上无设备写入检查：`python3 tools/check_update.py`。它验证HTTP/1.1状态、准确长度、禁止重定向/压缩/chunked，并对重复下载校验SHA256。无正式Release时404属于预期空站点状态。

## 脱敏与验收

`tools/public_files.py`是Git初次导入和源码ZIP共享的白名单。`tools/check_public.py --tracked`检查跟踪文件、常见敏感格式和设备标识；本地还比对private JSON中的已知网络凭据，但不打印实际值。`--artifacts DIR`同时检查发布附件和ZIP内容。检测器不能取代对新配置的人工审查。

验收按构建写入[VALIDATION.md](VALIDATION.md)，区分本机构建、Actions构建、线上HTTP、浏览器和实机。源码中的既有Wi-Fi/电池等修复进入本次新镜像，但历史实机记录不等同于0.5.1实机验收。
