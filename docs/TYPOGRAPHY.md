# 字体与排版

YA继续使用480×480物理像素原生绘制。字体采用2×设计单位：12单位正文直接栅格化为24px字形；不先绘制240×240图片再放大，不增加960×960帧缓冲。当前工作是统一字体与字号层级，没有将现有全部布局坐标迁移为240×240坐标系。

## 角色

通过 `main/typography.hpp` 复用以下字体，避免各应用自行决定全局字号。`typography::px(n)` 将设计单位转为物理像素，只用于新声明的设计尺寸；现有像素坐标不可重复缩放。

| 角色 | 设计单位 | 物理字号 | 字重与用途 |
|---|---:|---:|---|
| caption | 10 | 20px | Noto Sans SC + Inter Regular 400，说明、日志、列表副标题 |
| body | 12 | 24px | Noto Sans SC + Inter Regular 400，正文、菜单、输入框 |
| title | 14 | 28px | Noto Sans SC + Inter Medium 500，状态栏/弹窗标题、分组、重要按钮 |
| number_small | 10 | 20px | Inter Medium 500，电量、升级包大小、日志级别 |
| clock | 12 | 24px | Inter Medium 500，状态栏时钟 |
| brand | 14 | 28px | Inter Medium 500，启动YA标识 |
| number_large | 20 | 40px | Inter Medium 500，电池页大号数值 |

Inter按文本光学尺寸opsz=14实例化。所有字形为4bpp灰度、无运行时TTF解析。中文角色的ASCII也来自Inter；对应20/28/40px Montserrat仅作为既有LV_SYMBOL图标等缺字回退。动态姓名、SSID、文件名和日志仍受当前中文子集限制；蓝牙信息应用接收的浏览器排版像素不被本地字体重新排版。

字体像素字号与行高不同：当前20/24/28px字库行高分别为24/28/35px。窄列表、弹窗输入和按钮必须留足行高与内边距；状态栏长标题继续单行省略，正文继续纵向滚动。屏幕64px圆角、24px安全边距和触摸坐标沿用原契约。

## 生成与来源

首次运行 `./tools/bootstrap.sh --fonts`；已有字体缓存时运行 `.tools/venv/bin/python tools/fonts.py`。生成器从main和examples的cpp/hpp/h文件收集中文及符号，排除生成C文件与私用区图标。中文子集用于三个字号，保持页面切换时覆盖一致。原始TTF与中间实例放在被忽略的`.tools/fonts/`，只提交main内生成C资产和许可。

生成器固定lv_font_conv=1.5.3，并校验原始字体SHA-256。Noto Sans SC固定到google/fonts提交`2894aab31764f10f29c421bdfd2340d3b382d384`，Inter固定到官方v4.1。具体下载URL及哈希见`tools/fonts.py`。每次生成的字号、字重、回退和输出摘要记录在本地`.tools/fonts/generated.json`。正式构建直接编译已提交字库，不依赖字体下载。

- Noto Sans SC：[官方来源](https://github.com/google/fonts/tree/2894aab31764f10f29c421bdfd2340d3b382d384/ofl/notosanssc)，[完整OFL许可](FONT-LICENSE.txt)。
- Inter 4.1：[官方来源](https://github.com/rsms/inter/tree/v4.1)，[完整OFL许可](INTER-LICENSE.txt)。

字库裁剪/转换产物随软件分发时保留上述版权和许可；生成C文件附有许可路径。实际字号观感、低亮度可读性及实体触摸/圆角遮挡需要板级验收，离线渲染不能代替这些检查。

## 离线预览

`tools/typography_preview/`使用项目固定的LVGL源码、生成字体和实际boot_view/ota_view进行主机渲染。字体样张是排版示例，升级和启动页面调用实际视图；不会连接或改动设备。LVGL颜色深度与设备一致为RGB565，输出RGB888 PPM用于查看。

```sh
export PATH="$PWD/.tools/venv/bin:$PATH"
cmake -S tools/typography_preview -B build-typography-preview -G Ninja
cmake --build build-typography-preview -j 8
mkdir -p artifacts/typography
cd artifacts/typography
../../build-typography-preview/typography_preview
```

预览检查字形与图标回退、时钟/100%电量/紧凑按钮文字尺寸、输入行高、双行RSSI、升级方式同排与安全边距、常规升级页高度和启动层退出清理。不会把它当作完整Wi-Fi表单、键盘或实际触摸验收。
