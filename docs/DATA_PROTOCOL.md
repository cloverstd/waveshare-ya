# YA 统一数据协议与多网关设计

状态：0.3.0 源码实现与主机验证阶段；硬件验收未完成。2026-10-29。

实现入口和实际边界见 [Chrome 数据站使用说明](COMPANION.md)。下文保留目标合同，不能把设计中的所有硬件条件视为已通过。
面向 ESP32-S3-Touch-AMOLED-2.16。本文定义 YA Data Protocol v1，独立于现有 [BLE 管理协议 v1](BLE.md)。休眠与预算见 [省电设计](POWER_MANAGEMENT.md)。文中的数据服务现已由新源码和本地网关提供；线上服务器未部署；本次连接的设备已安装新固件并完成启动确认，数据链路仍待实测。

## 系统职责与应用透明性

```text
应用 → 系统数据服务（资源、缓存、订阅、命令、权限、同步计划）
     → 路由/网关管理
       ├─ Wi-Fi → HTTPS 统一数据服务
       └─ BLE → 网页/手机 App → 同一统一数据服务
```

第一版由统一数据服务适配第三方天气、行情和控制接口。网页/App 是受信任的转发网关，不在每个平台分别定义业务模型。此协议不是任意 URL 代理，不实现通用互联网隧道或图片流传输。

应用不持有 BLE/Wi-Fi 句柄、不选择链路、不处理重连。服务身份、账号范围、资源参数共同确定资源；城市、市场、账号不同的数据不能共享缓存。

| 系统能力 | 行为 |
|---|---|
| get | 异步读取，cache_first 或 refresh；返回缓存快照和刷新状态 |
| subscribe / unsubscribe | 逻辑订阅资源，系统根据活动/休眠状态调度 |
| command | 带幂等键的业务控制 |
| command_status | 查询执行状态不确定的动作 |
| cancel | 取消排队工作/停止等待，不承诺撤销已发生动作 |
| sync_plan | 系统持有的后台资源计划，与页面生命周期独立 |

应用退出注销 UI 订阅，旧 generation 结果不得访问已销毁对象。获用户启用的后台计划继续存在。网络、解析、持久化不占 GUI 线程；系统通过有界队列发布事件，业务只处理资源和统一错误。链路类型仅作为系统诊断信息，不是应用必填参数。

## 消息与资源合同

消息为 UTF-8 JSON，协议标识 proto="ya.data"，v=1。未知主版本拒绝；未知可选字段忽略，未知操作明确拒绝。单消息不超过 4096 字节，JSON 容器最大深度 6（根对象算 1）；禁止重复键、非法 UTF-8、非有限数字。大结果分页，每页仍服从上限，不能无界累积。

| 字段 | 规则 |
|---|---|
| proto / v / type | 固定标识、版本；类型 hello/request/response/event/ack/cancel/error |
| id / reply_to | 消息 UUID 字符串；响应、确认引用原消息 id |
| session | 本次连接握手建立的随机会话 ID，重连更换 |
| service / scope | 稳定服务身份与账号范围，必须匹配设备授权 |
| resource / schema | 资源名及业务 schema 整数版本 |
| op / params / data | 操作、参数和业务数据；请求不含伪造结果 |
| revision / cursor | 服务端不透明版本/游标，不按字符串大小判断新旧 |
| updated_at / ttl_s | 来源更新时间（UTC Unix 秒）和有效期秒数；未知时间用 null |
| idempotency_key | command 必填，同一次用户动作跨重试、换路保持不变 |
| status / error | 请求结果与结构化错误；仅成功入队不是动作执行成功 |

资源键由 service、scope、resource、schema 及排序规范化的 params 形成；只排序对象键，数组顺序有意义。缓存匹配不能只比较 resource。事件携带资源键对应参数及订阅 ID。hello 不要求资源字段；消息字段按类型验证而非全部强制出现。

hello 协商支持版本、最大消息长度、资源/schema 能力、订阅能力、幂等/状态查询能力和服务身份。有效消息上限取双方最小值。可信时间只能来自系统批准的时间服务；普通业务更新时间不用于设置系统时钟。

### 示例

以下示例展示业务请求与快照。真实传输每条消息必须补齐 proto、v、type、id、session、service、scope；响应还需 reply_to。此处的资源 schema 均为 1。

天气请求：

```json
{"resource":"weather.current","schema":1,"op":"get","params":{"location":"CN-101010100"}}
```

对应 data：

```json
{"temperature_c":18.5,"condition":"cloudy","humidity_pct":65,"timezone":"Asia/Shanghai"}
```

报价请求：

```json
{"resource":"market.quote","schema":1,"op":"get","params":{"market":"XSHG","symbol":"600000"}}
```

对应 data：

```json
{"price":"10.25","currency":"CNY","change_pct":"1.20","market_state":"open","delay_s":0}
```

价格/百分比使用十进制字符串避免浮点歧义；delay_s 未知为 null，不能默认当成实时行情。行情、天气的 updated_at 与 ttl_s 位于消息外层；不可用业务字段用 null，不用零替代缺失。

控制请求：

```json
{"resource":"device.switch","schema":1,"op":"command","params":{"device_id":"lamp-1"},"data":{"on":true},"idempotency_key":"b09a2528-722b-461d-80cc-1bbc2df44ab8"}
```

此处使用“设为开”的确定性操作，不默认使用 toggle。结果 status 为 accepted/running/succeeded/failed/unknown；succeeded 要有执行方确认。取消请求不能把已经开灯解释为已撤销。

### 一条完整读取消息

```json
{"proto":"ya.data","v":1,"type":"request","id":"ad9cb1ef-9c02-42d6-b080-9334e4611ed4","session":"9ee685aa-75ef-4387-91ed-4db099fded96","service":"home-data","scope":"account-a","resource":"weather.current","schema":1,"op":"get","params":{"location":"CN-101010100"}}
```

服务端响应必须匹配资源身份及请求；不接受网关将另一个账号的成功结果替换进来。

## BLE 与 HTTPS 映射

### BLE 数据服务

预留 UUID：

| 项目 | UUID |
|---|---|
| 数据服务 | 98f20001-8f7c-4cb1-9e50-6bf589c91010 |
| RX，Write with response | 98f20002-8f7c-4cb1-9e50-6bf589c91010 |
| TX，Notify | 98f20003-8f7c-4cb1-9e50-6bf589c91010 |

保留原 98f10001 管理服务、原 1024 字节扁平 JSON 与原指令；新数据解析器独立、有限深度，不能放宽旧协议检查来兼容新协议。

数据帧为 4 字节大端消息体长度加 JSON。长度不含头部；零长度及超过协商上限立即拒绝并清空该连接缓冲。每连接每方向只组装一帧，不交错分片；最多保留一个未确认发送帧，使用串行队列背压。控制确认不要求再次确认，避免确认循环。

客户端按 MTU-3 分片；Web Bluetooth 未知 MTU 时用 20 字节。接收连续 5 秒无进展丢弃半帧。ATT 写成功仅证明分片被接收；完整消息 ack 只证明解析接收，业务结果由 response 或后续状态返回。通知不是可靠队列：发送端保留完整帧直到 ack，超时重发同一 id；最多重发两次，且受请求/后台总截止时间约束。接收端按 session/id 去重，不重复交付。

设备 TX 发请求 → 网关 HTTPS 取数 → 网关 RX 回响应。设备 RX 可接收活动网关的推送；候选仅能握手与探测，不能执行命令或发布业务数据。原始 GATT 回调只复制和入队，不阻塞执行 HTTP、Flash 或 GUI 工作。

### HTTPS

配置 base_url，调用 POST /api/v1/data/exchange，每次一个相同 JSON 消息，不含 BLE 长度头。响应为一条消息；错误应包含协议错误体，非协议 HTTP 错误统一映射。HTTPS 必须校验服务器身份，账号授权不能只依赖客户端填写 scope。

第一版订阅映射为使用 revision/cursor 的有界轮询，返回一个资源快照或变化页；BLE 可推送相同事件。网络恢复先取快照再恢复游标，游标失效时重新全量读取。网页跨源访问需数据服务配置允许来源；第三方密钥只放服务端，不放网页静态包。

网关必须证明服务及账号访问能力；握手自报 online=true 不算可用，需要实际服务探测。配对只建立本地信任，不自动授予所有服务/账号权限。

## 缓存、错误与命令一致性

缓存由系统持有，状态 fresh/stale/missing，附 refreshing、last_error。fresh 必须具有可信时间依据且未过期；断网仍可返回旧值，但明确 stale。不同传输方式不改变单位、schema 和错误含义。

每次路由改变递增本地 generation；旧 generation 的结果丢弃。revision 是服务端不透明标识，不能直接比较大小：同资源刷新串行化，事件按订阅序列处理；序列断档先恢复快照。队列满合并可替换的数据快照，控制结果不可静默丢弃，返回 busy。

错误码至少包括 offline、timeout、unauthorized、unsupported、busy、protocol_error、stale、unknown_result。错误包含可重试标记；重试仍须遵守请求性质与总预算。

- get 可在剩余期限内换路重试。
- command 的幂等记录由统一服务保存，作用域为账号与资源；保留至少 24 小时，声明保留期限。过期键不能安全重放。
- 网关不能作为唯一去重权威，否则换手机或改用 Wi-Fi 会重复执行。
- 发送后断线先 command_status；执行方不支持可靠去重/查询时返回 unknown，不自动换路重发。
- 深睡前取消未发送普通请求，保存未决命令的最小追踪记录。醒来查询，不重放控制动作。
- ACK、accepted、succeeded 三者语义严格分离；协议不承诺无条件 exactly-once。

系统诊断记录当前状态、唤醒原因、下次同步、路由/切换原因、选优 RSSI、请求耗时及错误码，不记录凭据或完整敏感数据。缓存额度、队列和分页均有界，资源不足返回 busy；不通过无限增加内部 RAM 缓冲解决拥塞。

## 多网关配对、发现与选优

对象为手机/电脑联网网关，不是传感器。沿用当前总计 6 个绑定记录容量，记录满时由用户删除，不自动淘汰。保留六位数配对窗口、加密与认证要求；绑定身份/IRK 解析作为身份依据，名称和随机地址不能充当认证。忘记设备同时撤销网关权限和活动租约。

浏览器是 BLE Central，不能作为广播外设被设备扫描。设备广播，获授权且正在运行的网页/App 主动发现、重连并报名；首次浏览器授权必须由用户操作触发。只能选“本轮实际连上并完成认证的候选”，不能保证发现附近所有已配对手机。浏览器关闭或 App 被系统暂停时视为不可用。

目标资源：2 条已有传感器连接 + 1 条活动网关 + 1 条临时候选，共 4 条。当前配置总数是 3，需要未来修改容量并验证控制器限制及内部堆。未通过前保留现有行为，不断开传感器腾位置，不宣称全并发已支持。

选优过程：

1. 保留主连接并继续广播接受候选，候选槽每次只服务一个；完成/超时后释放。
2. 候选必须认证、匹配服务账号、支持目标资源并通过真实取数探测。
3. 在约 3 秒中采集 5 次设备侧连接 RSSI，以中位数比较，不信任客户端自报值。
4. 首次有限窗口内选有效候选中最强者；候选按重连退避参与，未能进入窗口的不算已比较。
5. 健康连接每 5 分钟最多选优一次；轻睡只在同步窗口或故障时探测。轮次受同步预算限制。
6. 健康切换需候选至少强 8 dB，连续两组样本满足条件；切换冷却 60 秒。故障不受滞回限制。
7. 新网关就绪后颁发新的活动代次，转移路由并释放旧连接；任何时刻只有一个业务网关，旧结果不能回灌。
8. 无有效网关则按允许策略回退 Wi-Fi。强信号但无法联网的候选不优于真正可提供数据的候选。

候选握手失败后使用带随机抖动的客户端指数退避（1、2、4、8、最高 30 秒）；成功后重置。设备对未认证连接限时断开，新绑定仅在用户配对窗口内。轻睡 BLE 接收不等于永不深睡，连接活动不重置用户闲置计时。

手机 App 需分别实现平台权限、后台模式和状态恢复；不得承诺 iOS 无限后台联网或任意时刻重连。普通数据默认静默，只有已授权通知能够申请亮屏。

## 验收与实施边界

先单网关验证两种传输返回同一资源及错误，再验证多网关和休眠联动。必验：

- 旧管理客户端正常，新协议能力协商、未知版本和权限拒绝正确。
- 20 字节分片、边界长度、畸形 JSON、重复键、丢通知、重复帧、半帧超时、断线与背压。
- 缓存键隔离城市/市场/账号；过期数据不标实时，旧会话响应不覆盖新数据。
- 命令已经执行但回包丢失，跨 BLE/Wi-Fi 恢复不重复动作；不支持去重时返回 unknown。
- 6 个绑定记录容量与删除；隐私地址变化；候选争抢、信号抖动、强信号无互联网、跨账号拒绝。
- 2 传感器 + 主/候选网关共 4 连接的内存、DMA、吞吐和长期稳定性；失败则多网关能力不能标验收通过。
- 浏览器关闭、手机锁屏、App 被系统终止时可靠降级，且不承诺后台必达。
- 定时同步遵守总预算、默认不亮屏、深睡期间 BLE 不可达；普通数据不重置轻睡阶段期限。

实现及验证状态见 COMPANION.md 与 VALIDATION.md。四连接容量、实际配对与休眠唤醒保持未验证。

## 依据

- [Chrome Web Bluetooth：Central/GATT、HTTPS 与用户授权](https://developer.chrome.com/docs/capabilities/bluetooth)
- [Apple Core Bluetooth 后台限制](https://developer.apple.com/library/archive/documentation/NetworkingInternetWeb/Conceptual/CoreBluetooth_concepts/CoreBluetoothBackgroundProcessingForIOSApps/PerformingTasksWhileYourAppIsInTheBackground.html)
- [Android BLE 后台通信](https://developer.android.com/develop/connectivity/bluetooth/ble/background)

## 0.5.0 资源保存与活动网关修正（2026-10-30）

设备选定活动 BLE 网关后发送需 ACK 的 `event`：`resource="system.gateway"`、`schema=1`、`params={}`、`data={"active":true}`。该事件使用已协商的 session/service/scope；每次选中发送一次，沿用有界重传。旧网页可 ACK 后忽略；新网页在收到该事件后补发暂存文字，不把双向 hello 当作活动路由已选择。旧固件仍可通过非 system.health 的资源请求让网页识别活动网关。

保存资源后，无论屏幕是否息屏，均请求一轮立即同步；后续恢复原后台周期。数据接收不重置用户闲置计时，文字是否短时亮屏仍由已有 wake_resources 授权控制。

设备 ACK 等待上限25秒、单资源响应等待35秒、前台请求总期限45秒；息屏请求总期限仍55秒。预算覆盖4096字节消息按20字节/约100ms写入，以及网关8秒来源I/O；不再用3秒ACK或8秒资源响应预算误断慢分片连接。次数和总期限仍有界，控制动作不盲目重试。
