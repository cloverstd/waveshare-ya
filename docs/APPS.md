# 新应用开发

## 分层与生命周期

- board.cpp 是唯一硬件初始化入口；AXP2101 与触摸、音频共享官方 BSP I2C 句柄。
- core/network/bluetooth/ota 提供系统服务，UI 使用状态快照及异步命令。
- app_registry.cpp 注册应用 ID、中文名称、图标标识、能力位、create/enter/leave/destroy。
- ui.cpp 管理桌面、路由、统一主题、圆角根容器与息屏唤醒。

`create(parent)` 只创建 UI；`enter()` 启动应用自己的订阅；`leave()` 停止更新；`destroy()` 取消任务、定时器、事件订阅、文件句柄和持有的对象。系统服务由系统长期持有，应用不反初始化 Wi-Fi、总线或 LVGL。

所有 LVGL 调用在 UI 线程或官方 BSP GUI 锁内执行。事件回调不可直接阻塞 GUI，也不要在 GUI 锁中发起 HTTP、目录扫描或图片解码。后台工作通过有界队列把结果送到 UI，离开应用后应丢弃旧 generation 的结果。

系统事件基于 ESP Event Loop 的 `ya::EVENTS`，事件 ID：SETTINGS、BATTERY、WIFI、BLE、STORAGE、OTA。普通状态事件无 payload，收到后调用 snapshot() 取一致状态。通过 esp_event_handler_instance_register 注册，并在退出时注销。网络与设备操作使用 command()，返回 false 表示队列满；应提示用户而不是无限重试。

`setting(key,value)` 统一验证并保存 brightness、volume、mute、fps、sleep_battery、sleep_usb、wifi、ble。GUI 使用 `setting_async(key,value)` 或 `update_url_async(url)`，非零请求 ID 表示已入队；全局 UI 持续消费 `setting_result`，只有匹配当前控件请求 ID 的完成结果可以改变控件。离开页面也必须回收完成结果。同步 setting 仅供非 GUI 调用。旧 sleep 写入被拒绝。不要直接写全局状态或 NVS。每个应用的业务配置使用独立命名空间，并按当前 OTA 槽隔离，迁移规则同系统配置。

## UI 约定

480×480 逻辑画布；统一根容器 64px 圆角及裁剪；内容与控件安全边距 24px。顶部固定时钟、通知、连接状态；桌面不显示标题和标语，应用网格从 y=72 开始，内容区高 384px。其他页面的标题在顶部状态栏 y=28、屏幕水平中心的128px区域显示，应用名称取自注册表，长标题省略；正文从 y=72 开始，高384px。通知使用安全区内的底部浮层。正文使用剩余安全区域滚动。返回/主页通过实体按键执行，不再占用底部；已打开的应用根页返回桌面，设置子页返回设置，弹窗优先关闭。返回层级依据应用生命周期状态，不按诊断页面ID大小推断。按钮最小 44px。弹窗和键盘必须位于同一根容器，不创建绕过圆角的 screen/top-layer 内容。下拉列表也应保持安全边距。

刷新率是 15/30/60 的目标 UI FPS。静态页面不需要无条件全屏刷新；耗时处理分帧或放到后台。息屏只降低亮度，保留联网，首个唤醒触摸由透明屏障消费。

## TF 卡与相册

官方驱动使用 1-bit SDMMC：CLK GPIO2、CMD GPIO1、D0 GPIO3。挂载到 `/sdcard`，使用 FATFS，长文件名开启。未使用 GPIO41 作为 SPI CS。无硬件 CD 信号，UI 提供显式挂载/安全卸载。

storage_list(relative,offset,limit,callback,context) 分页枚举，每次上限100项；storage_read(relative,offset,buffer,size,&got) 每次上限64KiB。路径必须相对，不允许绝对路径、`..` 或反斜杠。操作持有 storage_mutex，安全卸载等待当前操作完成。目录回调只复制必要的名字，不在回调里递归调用存储服务。

相册建议读取 `photos/`，后台分页过滤 JPG/PNG，缓存有限张缩略图，按显示分辨率解码，使用 PSRAM；禁用全目录一次性加载。监听 STORAGE 事件，无卡显示插卡提示，读失败关闭句柄并回到可恢复状态。当前服务不自动格式化、不假定支持物理热插拔。

## Home Assistant 应用

业务应用持有 HA URL 和凭证（不写源码），后台通过 HTTPS REST 获取实体状态，长连接用 WebSocket 订阅事件。首次版本可选必要实体，显式显示“连接中/离线/数据过期”和最后更新时间。

例如读取 `sensor.room_temperature` 显示其值与单位，点击灯光按钮调用 `light.turn_on/off`，等服务器状态反馈再确认 UI，失败时恢复原状态。离线时不积压危险或重复控制指令，恢复连接后重新拉状态。不要用 PMU 温度代替室温。

相册与 HA 应用本期不提供完整产品功能，以上是接入约定。`examples/app_template` 是可加入构建的最小模板。

应用设置使用 `app_config_get(id,schema,data,&size)` / `app_config_set(id,schema,data,size)`，id 限 1–15 位字母数字、下划线或短横线，每个应用 blob 最多 4096 字节。系统按运行槽分别保存到 apps_a/apps_b，OTA 切换前复制到目标槽。schema 不匹配返回错误，迁移由应用显式处理；写入受升级互斥保护。不要自行使用不隔离的全局 NVS 命名空间存放需要支持回退的 HA 配置。

GATT 事件是唯一带 payload 的系统事件：`GattEvent` 包含连接、句柄、状态、UUID、范围与最多512字节数据。kind 1=读结果、2=写结果、3=服务、4=特征、5=描述符、6=通知；`BLE_HS_EDONE` 表示服务发现完成。复制需要保留的数据，事件内存仅在回调期间有效。有界事件队列满时可能丢弃遥测，应用应使用读取重试恢复状态。

为新增应用提供图标后，将描述符追加到注册表；桌面会按两列自动换行，并使用同一内容滚动区域。应用能力字段是开发元数据，不是安全隔离：所有应用都是随固件编译的可信代码。

注意栈与内存预算：Status快照约数KB，避免在嵌套调用中反复复制。GUI和Flash/NVS/OTA操作必须使用内部RAM任务栈。图像等大对象可以使用PSRAM，但DMA是否可直接访问要以外设能力为准；总空闲内存不等于可用内部连续内存。调试时同时观察internal_heap和internal_largest。

目录相对路径目前限制为239字节，读操作最多64KiB；FATFS长文件名开启，不承诺exFAT。实际TF卡兼容性须插卡验证。当前固件中文字体包含界面字符，动态SSID/设备名/文件名中的其他字符可能需要扩展字库。

## 接入最小应用模板

`examples/app_template/template.hpp` 导出 `ya::example::descriptor`，对应实现已纳入构建。要在桌面显示它，在app_registry.cpp引入该头文件，并将 `example::descriptor` 追加到apps数组；保留前两个系统应用的位置。新增自己的应用时，把实现加入CMake源列表，按模板提供生命周期函数，并补充对应字库字符。

照片应用的后台任务只传递文件名、缩略图数据及generation编号，不持有LVGL对象指针；退出页面后拒收旧generation结果。HA应用同样让网络线程只解析数据和投递消息，UI线程负责绘制。事件处理函数不要等待GUI锁，退出时先取消订阅和定时器，再清理任务持有的资源。

## 0.2.0 系统服务扩展

`system_prefs()` 返回电池/USB独立超时，`effective_sleep(battery)` 依据新鲜有效的USB状态选择，否则按电池策略。旧v1配置序列化保持不变；新偏好使用 apps_a/apps_b 中保留记录 sys_prefs，业务应用禁止占用此ID。升级复制时沿用目标应用配置先清理再复制，旧版本重试升级也不会复活失败目标中的新偏好。

系统状态后台采样当前连接SSID/RSSI、内部RAM/PSRAM、已挂载文件系统容量。RTC在同一个BSP I2C总线上，UTC存储，北京时间显示；SNTP回调只发信号，由服务线程写RTC。不在GUI中读取I2C或文件系统。

OTA状态有独立阶段与传输字节计数；传输100%后仍需校验和首次启动验证。离开OTA页面不终止升级。UI文字只有变化才更新，长通知的滚动动画不能因轮询被反复重置。

## 绘制与任务栈约束

0.2.0 对固定版本 LVGL 单独使用 -O2，并启用官方样式缓存。根容器保留真实圆角裁剪；只有完全处于24px安全矩形且根容器不透明、无变换的刷新区域，才在COVER_CHECK预处理允许从不透明内容开始绘制。外部区域仍使用原圆角遮罩。不要把此优化扩大到不受约束的自定义根容器。

Status 是数KB结构；尤其不得在app_main长期栈帧和嵌套OTA启动检查中仅为访问一个字段创建完整快照。用state_mutex短锁拷贝所需字段，释放锁后再执行I/O。tests/test_stack_budget.py 根据当前正式构建的GCC .su记录检查这两层预算，运行时USB状态提供main/diagnostics栈最小剩余字节数。

## 0.3.0 系统数据与休眠能力（硬件待验收）

新应用的统一数据接口设计见 [DATA_PROTOCOL.md](DATA_PROTOCOL.md)：应用使用资源读写、订阅及后台计划，不处理 BLE/Wi-Fi 路由。系统后台计划与页面订阅生命周期分离，退出页面仍须清理 UI 资源。当前 HA 直接 HTTPS 的示例约定仍是现有路径；0.3.0 使用 data.hpp 的异步接口，取数结果通过 data_view 查询，订阅资源使用 data_subscribe/data_unsubscribe；后台计划由 Chrome 配置持久保存。

[POWER_MANAGEMENT.md](POWER_MANAGEMENT.md) 定义轻睡、深睡、静默同步与休眠协调。本文前述“息屏只降低亮度”描述当前固件，不能与设计中的整机休眠混淆。

## 0.5.0 通知与状态界面

日志应用使用独立系统采集服务，运行期级别、A/B持久化、缓冲及界面生命周期见[日志应用](LOGS.md)。GUI通过现有`setting_async("log_level", 0–5)`修改设置，禁止在页面回调直接写NVS。

通知通过系统服务持有，应用只读取 `notifications_snapshot` 和提交 `notifications_read` / `notifications_clear`。最近10条保存在独立NVS命名空间notifications中，作为运行日志跨重启保留，不属于需要随OTA回退的业务配置。旧版本不会读取或重写该记录。文本上限159字节，UTF-8边界截断；连续5秒内相同事件合并。记录保留原事件UTC时间、开机标识和单调时长；未校时显示未知，同次开机校时后补齐。开始记录前的事件没有可恢复时间，不从旧OTA结果伪造历史通知。

服务任务每秒合并保存，正常重启、关机和深睡前checkpoint；突然断电可能丢失最后一个保存周期。清空按提交时的ID截止，只在NVS写入成功后从内存删除；并发到来的新通知保留。存储失败时保留历史并允许重试。UI不在GUI锁内访问NVS。通知应用通过注册表进入，页面诊断ID13；主循环负责仅在该应用打开时刷新，不创建额外任务或定时器。

系统状态使用电量圆环、网络摘要及内存/存储容量条；数据仍来自后台快照。右侧Wi-Fi、蓝牙、充电和电池采用Flex布局，隐藏充电标识自动收紧。桌面不再创建底部IP/OTA文本对象。通知角标采用红底白字；每次创建桌面时先隐藏角标，并在首帧前从通知快照设置数字和可见性，避免空红底短暂出现。打开通知应用时标为已读；短暂提示约5秒，不再被连接或OTA持续状态占用。

## 当前开发版的存储与信息服务

设置采用同页分组，新增电源页面诊断ID15。`power_battery_delay`/`power_usb_delay`（0–1440分钟，0立即）经异步设置保存到槽隔离记录`sys_delay`，不改变旧电源配置布局。

内置SPIFFS首次挂载失败后完整检查分区，仅全部字节为0xFF才自动初始化；读失败或任何既有内容都不格式化。`FLASH_RETRY`通过原服务队列重试，状态带flash_note/flash_error，TF卡仍显式挂载、安全卸载。

信息应用使用companion系统服务提供的不可变共享图像快照，不在GUI中解压或读文件。最近文字原文和图像以版本化双份缓存保存在内置存储，包含头部CRC32及压缩包SHA-256；只更新另一份，校验新文件成功后才替换当前显示。断电/损坏回退到另一份有效缓存；旧固件忽略这些文件。缓存不改变A/B设置复制或回退规则。
