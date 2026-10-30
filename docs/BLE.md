# BLE v1 协议

设备名 `Waveshare-YA-XXXX`（XXXX 为硬件蓝牙地址末两字节，保留 Waveshare-YA 前缀），同时最多两个 central 连接（外部外设）和一个 peripheral 连接（手机/电脑）。BLE 开关控制扫描/广播/连接；ESP32-S3 不支持经典蓝牙音频。

| 项目 | UUID |
|---|---|
| 服务 | 98f10001-8f7c-4cb1-9e50-6bf589c91010 |
| RX（Write with response） | 98f10002-8f7c-4cb1-9e50-6bf589c91010 |
| TX（Read / Notify） | 98f10003-8f7c-4cb1-9e50-6bf589c91010 |

在屏幕打开两分钟配对窗口，使用系统配对提示输入屏幕六位数。新设备在窗口外拒绝连接；已绑定设备可重连。设置接口要求 encrypted + authenticated，不能在未配对链路写 Wi-Fi 密码。

请求为 UTF-8 JSON，以换行结束，最多1024字节，分片累计超时5秒。每次只保留一个请求；过大请求返回 ATT 错误并清空缓冲。客户端按 MTU-3 分片，或保守每次20字节写入。不要并发发送请求。

```json
{"v":1,"id":1,"op":"status"}
{"v":1,"id":2,"op":"set","key":"brightness","value":60}
{"v":1,"id":3,"op":"wifi","ssid":"example","password":"example-only"}
```

响应含 v、id、ok；失败含 error。status 返回固件版本、IP、电量、充电状态和显示/声音设置；wifi 的 ok 表示连接请求已入队，不代表已获得 IP，后续查询 status。通知按协商 MTU 分片，以换行结束；通知丢失可读取 TX 获取最后结果。

使用 `tools/ble_client.py` 做验证，密码通过 getpass 输入，不进入命令历史。macOS 会自行管理 BLE 配对交互，不保证显示在传统蓝牙设备列表中。

Central API：ble_read(conn,value_handle)、ble_write(conn,value_handle,data,size)、ble_subscribe(conn,cccd_handle)。订阅需要 CCCD 描述符句柄，不能把 characteristic value handle 误当 CCCD；不同传感器需对应协议驱动。

连接列表最多保留16个较强信号的可连接设备，重新扫描会刷新未连接记录并保留活动连接，避免隐私地址变化造成陈旧记录。广播型、不可连接信标不在此连接列表中；其广播协议需要后续专用服务适配。界面操作绑定设备地址，列表刷新不会把点击映射到另一台设备。关闭BLE会取消正在发起的连接并断开活动连接。

0.1.1 的状态响应额外返回 mute、fps 和 sleep。设备支持同时最多两个外设和一个手机/电脑角色；当前实测覆盖Mac客户端和单个模拟温度外设，两外设并发仍需额外硬件验收。

请求和OTA清单只接受扁平JSON对象；不支持嵌套对象或数组。字符串内的括号不受影响。解析前检查结构，避免深度嵌套输入消耗嵌入式栈。USB诊断消息使用seq关联请求，page操作的id仍是页面编号。

## 0.2.0 新字段

status 增加 sleep_battery（30/60/300秒）、sleep_usb（60/300/600/0秒，0=常亮）、wifi_ssid、wifi_rssi（dBm）、time_valid、rtc_valid、last_time_sync（UTC Unix秒）及 ota_phase、ota_progress、ota_received、ota_total。旧 sleep 读字段为当前供电下生效的超时；旧 sleep 写请求返回 use_sleep_battery_or_sleep_usb，请分别设置两个新键。

ota_phase 为 idle/checking/available/current/transferring/verifying/rebooting/boot_verify/complete/failed/rolled_back 之一；进度只表示传输百分比，complete才表示新固件启动验证通过。time_valid=false时不要展示Unix初始时间；rtc_valid=false可能表示联网时间正常但离线保时不可用。

## 0.3.0 数据服务（硬件待验收）

[统一数据协议](DATA_PROTOCOL.md) 定义独立 GATT 数据服务、多网关选优及 BLE/Wi-Fi 统一资源语义。它不替换本文管理协议，不改变当前 1024 字节扁平 JSON 限制。0.3.0 源码已接入，目标 4 连接需实机验证；已安装的 0.2.0 不提供此服务。运行网页见 [使用说明](COMPANION.md)。轻睡 BLE 接收与深睡断连边界见 [省电设计](POWER_MANAGEMENT.md)。

## 0.5.0 连接阶段与诊断

配对记录只代表密钥已保存，不代表网页当前在线。状态按网关连接句柄分别保存encrypted/authenticated/subscribed/ready；旧phone_connected继续表示至少一个手机/电脑物理链路存在。USB status新增ble_links、ble_services_valid和ble_disconnect_reason；管理status保留旧字段并补充服务可用性和手机/电脑链路状态。ready来自system.health探测响应，断开一个外设不会清除其他活动网关状态。

NimBLE启动时检查98f10001管理与98f20001数据服务注册并记录句柄，通过Service Changed提示已订阅的绑定主机刷新旧服务表，不新增或修改UUID，不降低MITM/加密要求。已升级主机若仍无法发现数据服务，网页停止自动重试并提示核对固件、在系统中忘记该设备后重新配对；此恢复动作需要用户在系统界面完成。

网页先取得管理服务并订阅管理通知，触发安全协商，再读取受认证保护的TX、成功读取status后，再发现数据服务、订阅通知并完成双向hello。只有这些步骤全部成功才显示连接就绪。选择器取消显示未选择设备；服务缺失/认证失败/协议握手失败停止自动重试；暂时掉线按1～30秒退避重连。每次连接清理旧监听、计时器和请求，并用会话代号拒绝旧回调。

0.5.0 扫描响应广播数据服务98f20001，网页优先按该服务发现设备；管理服务98f10001仍可通过连接后发现。旧客户端的名称前缀筛选仍有效；按管理服务广播筛选的客户端需改用名称前缀或数据服务UUID。页面可填写完整设备名称进一步筛选，系统名称缓存尚未更新时留空。设备页与USB status的ble_name提供实际名称。管理通知丢失时，网页在受认证TX中读取最后响应恢复，不重复执行管理命令。

控制器在NimBLE恢复已绑定IRK后、开始广播前开启地址解析；原来的已绑定身份/配对窗口准入条件保留。连接回调不再立即主动发起SMP；网页先完成管理服务发现，再由管理通知订阅触发安全协商，等待受保护TX读取成功，所有管理读写和数据写仍要求加密与认证。未认证链路不会被标为蓝色已连接，数据就绪另行显示。

配对与数据会话有独立时限：GAP连接后首次hello前最多120秒，避免15秒数据超时打断验证码输入；首次hello后最多30秒完成数据握手。ready后重新起算候选网关窗口，避免慢配对完成后立即被当作过期候选清除。USB status补充data_route与ble_drop_note，区分握手、ACK、资源响应或候选清理导致的断开。

每个新会话重新获取特征并订阅通知。管理通知保留停止后重订阅的安全协商流程；数据通知对当前特征对象先 startNotifications 注册，再 stopNotifications → startNotifications 完整重订阅，避免新对象直接停止或平台缓存使实际 CCCD 未开启。网页握手上限25秒，设备数据握手上限30秒；配对等待另算120秒。
