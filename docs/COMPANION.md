# 蓝牙数据站：文字与设备日志

当前版本：`0.7.0`。源代码及主机测试已完成，蓝牙实传和实体屏幕尚待验收。数据站提供文字发送、相册管理、设备日志和连接诊断，天气、行情、统一数据源及网页休眠设置已移除；固件的既有统一数据服务保持兼容。

## 使用

运行 `python3 tools/data_gateway.py --port 8765`，用支持 Web Bluetooth 的 Chrome 打开 <http://127.0.0.1:8765/>；线上页面需要 HTTPS。设备开启 BLE 和两分钟配对窗口，选择屏幕上的唯一设备名，完成六位验证码配对。网页读取已认证设备的服务身份后协商数据会话，不需要重新保存资源配置。

输入文字并检查预览，点击“发送文字”。离线可暂存，连接恢复后发送；失败时内容保留在输入框，支持取消和重新发送。设备校验、解码、持久保存并更新信息应用的数据后才返回业务成功；ATT 写成功和传输 ACK 都不代表业务成功。传输进度到 100% 后仍需等待设备提交结果。

文字保留空格和换行，浏览器负责字形与 Emoji 排版；设备显示浏览器生成的 RGB565 图像。浏览器自身缺字会反映在预览中。限制为 1–1000 个 Unicode 码点、432 像素宽、2048 像素高、压缩后最多 128 KiB；超限不截断，提示缩短。通过 zlib 压缩原文和像素，BLE 分块发送，速度取决于实际链路。设备保存最近一条，重启/深睡后恢复；写入失败保留旧消息。设备通过滚动查看长内容。

设备“日志”工具栏的上传图标发送本次开机最近日志。网页“拉取最近日志”分页读取，勾选“实时接收”接收后续日志，“下载文本”导出当前显示内容。实时接收默认关闭，断线后需重新开启；网页最多显示1000条，设备仍仅保留100条。覆盖、采集争用丢弃和游标缺口分别显示。

普通推送与实时订阅不重置休眠倒计时，深睡不可达。手动日志传输最长2分钟，文字上传最长20分钟；仅在传输有效期间申请有限休眠保护。文字上传期间暂停日志推送，完成后按游标报告遗漏。网页关闭、电脑休眠或手机后台限制均可能中断连接。

## 协议扩展

沿用 `ya.data` v1、现有加密认证 GATT、4字节大端帧长度、4096字节 JSON 上限和20字节 Web Bluetooth 写分片。管理服务 `status.data_identity` 是包含 service/scope 的 JSON 字符串，不含访问令牌。双方 hello 增加能力标记，旧网页不会被选为日志上传目标；设备 hello 新增 `companion="text-image-v1,logs-v1"`。旧固件显示升级提示。

网页请求使用标准 envelope 的 `type=request`、`resource`、`schema=1`、`params={}`，扩展参数在顶层。业务结果使用 `response`、`reply_to`、`status=succeeded/failed` 和 `data/error`。同一 session/id 的重复请求重放有界缓存响应。

- `info.text`：`op=begin`，携带 transfer、height、text_bytes、packed_bytes、sha256；`op=chunk` 携带 transfer、offset 和 base64 bytes（每块最多1536字节）；`op=commit/cancel` 携带 transfer。
- 压缩输入为原文 UTF-8 字节，紧接宽432、行跨度864、RGB565小端像素；整体使用标准 zlib/deflate。sha256 覆盖整个压缩包。顺序分块允许相同偏移和内容的重发，乱序、超限、损坏、非法 UTF-8 或解压尺寸不符均拒绝。
- `system.logs`：`op=get/subscribe/unsubscribe`，读取可携带 boot 和 after 游标。不同开机标识自动从当前缓冲开始。日志功能需要协商消息上限至少2048字节。
- 日志响应/事件的 data 含 boot、entries（sequence、level、text）、next、more、overwritten、dropped、missed。设备发出的批次使用 `type=event, resource=system.logs`，沿用 ACK/重传。单批最多4条且满足协商字节限制。

没有额外 HTTP 上传或第三方文字渲染服务。原文和日志经已配对的 BLE 链路传输；网页不将内容上传至外站。

## 0.8.0 相册管理

新增独立`gallery-v1`能力，管理入口及异步任务接口见[GALLERY.md](GALLERY.md)。连接只支持旧能力的设备时，相册管理禁用，文字和日志继续可用。更新网站时必须一并部署gallery.mjs。
