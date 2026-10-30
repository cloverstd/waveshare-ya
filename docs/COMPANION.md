# Chrome 蓝牙数据站与 0.3.0 使用说明

0.3.0 已通过签名 OTA 安装到本次连接的设备并完成启动确认；蓝牙端到端与休眠硬件验收仍未完成。其他仍运行 0.2.0 的设备不会提供新的数据 GATT 服务。

## 启动网页

在项目根目录运行：

```sh
python3 tools/data_gateway.py --port 8765
```

Chrome 打开 <http://127.0.0.1:8765/>。服务只监听本机，无第三方前端依赖。直接双击 HTML 不是受支持的使用方式。页面需要支持 Web Bluetooth 的 Chrome、蓝牙适配器和系统蓝牙权限。

设备升级后，在设备打开 BLE，并进入两分钟配对窗口；网页点“连接设备”，选择 Waveshare-YA，在系统提示中输入屏幕六位数。已授权的设备在页面仍运行时自动重连。点击断开会停止自动重连。

网页默认直接请求 Open-Meteo 当前天气和未来三天预报；经纬度初始值是北京示例，不读取浏览器定位。更新手动行情后数据含 source=manual、delay_s=null、60 秒有效期，不表示自动实时行情。文字可以主动推送；默认不亮屏，“允许文字通知亮屏”需要保存到设备才生效。

点击“保存资源到设备”设置服务身份、账号范围和天气/股票/文字三个后台计划。配置异步入队，成功保存后断开再重连。管理帧上限仍为 1024 字节，URL/令牌过长时页面会拒绝，不能将 ACK 当作持久化成功；查询 data_status 检查配置错误。

## 两种数据源

1. 浏览器直连：天气直接从 Open-Meteo 取得，股票/文字由页面提供。页面关闭或休眠后不可用。设备 Wi-Fi 无法访问浏览器内存中的手动数据。
2. 统一服务：页面转发到配置的 `/api/v1/data/exchange`；设备 Wi-Fi 直接访问相同地址，身份与资源一致。设备地址必须 HTTPS，证书须受 ESP-IDF CA bundle 信任；localhost HTTP 只用于本机网页测试，设备中的 localhost 指向设备自己。

该 Python 服务已经提供同一 exchange 端点、天气适配、外部快照和可选上游转发。公网使用应在受信任 TLS 反向代理后部署，不需要也不应关闭设备证书验证。跨源前端须由部署方设置限定来源的 CORS；默认推荐同源。

服务环境变量：

| 变量 | 用途 |
|---|---|
| YA_DATA_SERVICE / YA_DATA_SCOPE | 默认 ya-local / default，与设备和页面一致 |
| YA_DATA_TOKEN | exchange Bearer 令牌；非 loopback 监听必须设置 |
| TWELVEDATA_API_KEY | 可选自动报价来源，只保存在服务器 |
| YA_RESOURCES_FILE | 可选资源快照文件，每个文件最多 4096 字节 |
| YA_UPSTREAM_URL / YA_UPSTREAM_TOKEN | 可选统一服务上游，必须 HTTPS；控制去重由该执行权威负责 |

令牌只保存在网页本次内存；点保存资源时会经认证 BLE 写入设备槽隔离配置。不会保存到浏览器 localStorage，不打印在日志里。设备未启用 Flash 加密，不宣称凭据具备物理防读保护。

自动报价使用 Twelve Data `/quote`，传入证券代码和市场 MIC（如 XNAS/AAPL）。需自行提供有相应市场权限的 API key，返回市场不匹配则拒绝；保留服务来源时间，delay_s 未知，缓存最多每分钟抓取一次。未提供 key 或资源快照时返回 missing，不伪造价格。供应商额度、市场覆盖及延迟取决于实际账号，本次没有该 key，自动报价只做了模拟响应适配测试。

快照文件为对象，键是 `[resource, params]` 的紧凑、参数键排序 JSON；值为 `{data, updated_at, ttl_s}`。updated_at 是数据真实的 UTC Unix 秒，读取时不重写为当前时间。示例键：

```text
["market.quote",{"market":"XNAS","symbol":"AAPL"}]
```

内置服务不执行设备控制，返回 unsupported。使用控制能力须配置支持 command、稳定幂等键和 command_status 的统一上游；网关不盲目重试控制动作。

## 设备操作

桌面新增“信息”应用，显示天气、预报、报价、文字、来源、更新时刻和过期标记。应用不处理连接；底层取数、缓存和切换由系统数据服务负责。

显示设置可选电池/USB 休眠策略；网页可以设置两种供电的策略、1～1440 分钟轻睡阶段、0～1440 分钟同步周期及联网优先级。电池默认轻睡 10 分钟再深睡，每 15 分钟同步；USB 默认仅息屏在线。

整机轻睡后当前只承诺实体按键唤醒的实现路径；触摸唤醒未启用。深睡不能 BLE 唤醒。系统数据不重置用户闲置计时；被授权的文字通知最多亮屏 10 秒，通知限流当前是全局每 60 秒一次（比设计的逐应用限流更保守）。

缓存最多 8 个资源；深睡快照总计最多 4096 字节，超出部分丢弃，下次标记缺失并重新取得。关闭页面不会删除后台同步计划。用户的 Wi-Fi/BLE 禁用开关优先于路由；系统节电启停不改持久开关。

## 构建与验证

```sh
./tools/idf.sh -B build-data -D SDKCONFIG=build-data/sdkconfig -D PROJECT_VER=0.3.0 -D YA_FAULT=0 -D YA_HTTPS_FIXTURE=OFF build
node --test tests/protocol.test.mjs
.tools/venv/bin/python -m pytest -q tests
```

SDKCONFIG 使用新的构建内配置以加载省电默认值；直接复用旧 sdkconfig 会保留其关闭 PM 的旧值。日常更新只使用签名应用镜像，不使用 merged/bootloader/分区镜像；不能据此文档自动视为已获得刷写授权。

四连接目标为 2 传感器 + 活动网关 + 候选网关，配置已改为 4，硬件容量未验收。浏览器只能主动报名，不能被设备扫描成外围设备。信号选优、重连与备用网关依赖配套客户端正在运行。

参考：[Open-Meteo API](https://open-meteo.com/en/docs)、[Twelve Data API](https://twelvedata.com/docs)、[Chrome Web Bluetooth](https://developer.chrome.com/docs/capabilities/bluetooth)。

安装阶段已修正内部内存能力预算：长期工作栈静态保留在内部RAM，DMA专用预留池32KiB，关闭Wi-Fi IRAM速度优化，并在初始化/亮屏期间持有防轻睡锁。细节和实际指纹见 [验收记录](VALIDATION.md)。

## 0.5.0 网页连接说明

升级后重新加载网页以载入connection.mjs。连接栏按设备选择、安全验证、数据服务发现、通知订阅和数据握手显示进度；系统选择器中的已配对与网页连接就绪是不同状态。不要把User cancelled the requestDevice() chooser.用于判断之前的配对是否成功；页面会将这次选择取消显示为未选择设备。服务不匹配会停止重试并给出恢复指引，手动断开会立即取消自动重连。

主机网页检查：`node --test tests/protocol.test.mjs tests/connection.test.mjs`。通知时间按设备北京时间显示；最近10条跨重启保存，进入通知应用标已读，清空只删除提交时已存在的记录。

0.5.0 设备名称显示为Waveshare-YA-XXXX，可对照设备蓝牙页。网页默认按YA数据服务查找；完整名称筛选为可选项，旧系统名称缓存未更新时请留空。Unsupported device 属于系统设备记录不可用，页面停止重试并提示系统侧重新配对。网页只在本次内存保留设备选择与访问令牌。

重连保留原设备授权；支持watchAdvertisements的浏览器会先等待同一设备重新广播再连接，手动断开会取消等待。首次选择直接使用选择器返回的设备句柄。实际macOS新配对需要及时在系统提示中输入设备屏幕六位码；页面会在安全阶段等待，只有管理状态和数据握手完成后显示连接就绪。

ACK截止时间从全部分片GATT写入完成后开始，默认6秒；慢分片不会提前触发重发。数据握手传输超时有界重连最多三轮，实际协议不兼容仍停止。

## 资源保存与文字送达（2026-10-30）

资源保存只在连接就绪时可用，提交后锁定到下一次会话就绪，避免在重连握手中再次提交配置并触发第二次主动断开。保存后的预期断开单独提示；仍可通过手动断开停止自动重连。管理命令与数据分片共用串行 GATT 操作队列，会话的 service/scope 在 hello 时固定，编辑表单不会改变活动会话身份。

“更新文字”在未连接、保存配置或重连期间先存入本页内存，显示“文字已暂存”。设备通过 system.gateway 通知本网页成为活动网关后，网页补发最新文字（旧固件通过资源请求识别活动网关）；收到设备传输确认后显示“文字已发送，设备已确认接收”。收到 info.text 请求时也可直接通过资源响应送达。失败的文字保留待重连重试；刷新或关闭页面仍会清除本页内存。设备必须已经保存包含 info.text 的资源计划。

网页回归：`node --test tests/protocol.test.mjs tests/connection.test.mjs tests/companion.test.mjs`。ACK timeout 本身不能证明无线距离过远；应结合设备 ble_drop_note、链路阶段与数据路由诊断。

数据通知在每个会话对当前特征对象执行 startNotifications → stopNotifications → startNotifications：先注册当前对象，再完整重订阅。实测新对象直接停止/开启，以及只调用 startNotifications，均可能使 Chrome 声称已订阅、设备 subscribed=false，握手 ACK 无法回传；250ms 间隔候选也未解决。最终三步流程另行实机复测，管理通知的安全协商顺序保持原流程。

设备修复固件同时让资源保存触发息屏下的一轮立即同步；不必等待下一个定时周期。设备端慢分片与资源响应预算见 DATA_PROTOCOL.md。

## 在线入口

Cloudflare Workers 部署后使用 <https://ya.cloverstd.com/companion/>，静态资源直接来自本仓库 `web/`，无需启动本地 Python 服务即可使用浏览器直连模式。统一数据模式仍须配置实际数据服务地址；站点不代管用户令牌或手动数据。
