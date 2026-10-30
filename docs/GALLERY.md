# 相册（0.8.0）

ESP32-S3-Touch-AMOLED-2.16 专用。桌面打开“相册”，选择命名相册后显示3×3缩略图。每页最多9张，底部箭头或左右滑动翻页；点击照片进入大图，左右滑动首尾循环，当前图保持到下一图加载完成。大图等比完整显示，缩略图居中裁剪。返回键依次返回九宫格、相册列表、桌面；主页键直接退出。没有自动播放或双指缩放，息屏沿用系统设置。

## 管理

运行 `python3 tools/data_gateway.py --port 8765`，在支持 Web Bluetooth 的 Chrome 打开 <http://127.0.0.1:8765/>。线上数据站仍为 <https://ya.cloverstd.com/companion/>，需部署包含相册模块的网站版本后才有新入口。按既有流程完成设备配对；“相册”区用于创建、命名、排序、删除相册和照片。网页相册名称最多32个Unicode码点、UTF-8最多96字节，设备文字显示仍受当前字库子集限制。旧固件保留文字、日志能力，相册管理按钮禁用。

全设备最多8个相册。创建时选择来源，后续可以改名，不能直接改变来源：

- **本地照片**：每册最多9张；网页上传JPG/PNG，自动应用方向、等比缩放至432×384以内、合成黑色背景并压缩为最多128 KiB的JPEG。设备重新解码、规范化、保存并校验完成后才显示成功。上传支持逐张替换，也可多选顺序上传；多图是逐张提交，后续失败时已成功的照片保留。
- **图片 URL 列表**：网页添加图片直链及可选缩略图直链，支持替换、删除和排序。
- **远程 JSON 清单**：网页填写清单URL，由清单确定照片与顺序；照片只读，修改清单后点击网页刷新，或在设备重新进入相册。最近60秒已尝试刷新时复用当前清单；失败保留上次成功清单。

在线照片合计最多90张，包括清单已经接受的照片；超限整次更新失败，不截断。URL最多512字节，支持HTTP/HTTPS，不接受URL内用户名/密码及片段；HTTPS验证系统证书，最多3次重定向，不允许降级HTTP。单次下载总时限30秒，单次I/O超时1秒。URL不会作为错误日志输出。

```json
{"schema":1,"photos":[{"id":"photo-1","url":"https://example.com/photo.jpg","thumbnail_url":"https://example.com/thumb.jpg"}]}
```

清单最多128 KiB；`id`在相册内唯一，由1–64位英文字母、数字、下划线或短横线组成。`thumbnail_url`可省略。图片由设备Wi-Fi直接读取，不经网页或第三方代理。在线像素仅在PSRAM缓存，重新启动或释放缓存后需联网；已下载的有效清单会持久保留。

设备直接读取的在线图片和TF原图支持基线JPEG、非交错PNG，单图最多1 MiB、宽高均不超过2048；处理JPEG EXIF方向，PNG透明像素与黑色背景合成。渐进JPEG、交错PNG或更大的照片可通过网页上传，由浏览器转换。网页不修改原文件。

## TF 卡导入

先在设备“设置 → 存储”显式挂载TF卡，照片放在 `photos/`。在网页读取目录，选择文件导入当前本地相册；子目录可在路径输入框输入如 `photos/trip`。目录每次8项，使用“下一批”继续。目录项也可能是子目录或其他文件，非图片导入会明确失败。

导入复制并缩放压缩到内置存储，不改变TF原文件；之后移除TF仍可看图。替换时先在照片列表点击“替换”，再选择TF照片或网页文件。设备不自动格式化TF卡，不承诺热拔兼容。

## 内置存储与回退

内置SPIFFS与信息应用、原厂既有文件共用。每册9张是数量上限，不能保证8册各9张都能装满；网页收到容量错误后可删除不需要的相册照片。按实际挂载容量保留25%且至少512 KiB空间，不以分区表大小代替实际卷容量。原厂3 MiB卷兼容行为见[STORAGE.md](STORAGE.md)。

相册目录与照片使用独立 `ygc_` / `ygp_` 前缀和内容哈希；写入、同步并读回校验后，最后提交`app_config("gallery",schema=1)`中的小型目录引用。目录与照片的不可变对象存入SPIFFS；NVS引用按apps_a/apps_b隔离并随OTA复制。回收同时读取两槽引用，保留回退需要的目录和照片；引用未知或损坏时停止回收。仅回收相册自有对象，不清理信息缓存、原厂文件或TF文件。回退槽持有的旧照片可能暂不释放空间，故容量以设备实际结果为准。

目录损坏不会初始化空目录覆盖原记录；设备显示校验失败，管理请求失败，保留数据供诊断。真正Flash断电一致性、硬件读写可靠性仍须实机验证。

## 服务与协议

应用通过registry的create/enter/leave/destroy及可选back回调接入。独立内部RAM栈的相册服务执行网络、文件、解码；GUI只读不可变快照，任务不持有LVGL对象。退出使generation失效并异步释放浏览缓存。正在网页管理的任务由系统服务持有，独立于设备相册页面；有效上传申请有限休眠保护，浏览不续期亮屏。深睡等待相册后台操作结束。

沿用认证加密BLE和ya.data v1，能力增加`gallery-v1`，相册请求要求协商4096字节消息。所有下列请求使用`schema=1, params={}`：

|资源|操作|关键字段|
|---|---|---|
|gallery.albums|list / create / update / delete / move / refresh|offset；album；name、source、manifest；position|
|gallery.photos|list / add / update / delete / move|album、offset；photo、url、thumbnail_url；position|
|gallery.transfer|begin / chunk / commit / cancel|transfer、album、可选photo、size、sha256；offset、base64 bytes|
|gallery.sd|list / import|path、offset；album、可选photo|
|gallery.jobs|get / cancel|job_id|

除jobs外，成功响应中的`data.job_id`仅表示任务入队；网页轮询jobs直到`state=done`并读取result，或`state=failed`读取error。列表每批2个相册/照片，带total；TF列表每批8项。一次最多8个排队请求，保留最多16条任务记录。上传块最多1536字节，允许相同内容的重复偏移，不接受乱序或冲突重发；45秒无活动、20分钟总时限、连接断开或OTA开始会取消未完成上传。取消是尽力而为，已完成的持久提交不撤销。已有文字/日志协议保持兼容。

## 离线界面验收

`tools/gallery_preview`编译实际`gallery_app.cpp`，用固定LVGL及生成字体运行相册列表、九宫格、10张分页、大图、实体返回回调和50次生命周期循环；像素为合成图，不是设备下载/解码结果。

```sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S tools/gallery_preview -B build-gallery-preview -G Ninja
cmake --build build-gallery-preview -j 8
mkdir -p artifacts/gallery
cd artifacts/gallery
../../build-gallery-preview/gallery_preview
```

截图是LVGL软件渲染，不证明实机触摸、动画速度、面板显示、照片解码或TF兼容性。
