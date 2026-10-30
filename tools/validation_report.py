#!/usr/bin/env python3
"""Generate an evidence-based report; refuses incomplete soak data."""

import json, pathlib, datetime, hashlib

root = pathlib.Path(__file__).resolve().parents[1]


def load(name):
    return json.loads((root / "artifacts" / name).read_text())


def series(name):
    return [json.loads(l) for l in (root / "artifacts" / name).read_text().splitlines()]


a = series("soak.jsonl")
b = series("patch-soak.jsonl")
assert a[-1]["elapsed"] >= 7200 and b[-1]["elapsed"] >= 900
assert "SOAK PASSED" in (root / "artifacts/soak-progress.log").read_text()
assert "SOAK PASSED" in (root / "artifacts/patch-soak-progress.log").read_text()
patch = load("patch-regression.json")
expected = (root / "build-patch/waveshare-ya.bin").read_bytes()[176:208].hex()
assert (
    patch["after"]["build_sha256"] == expected
    and b[-1]["state"]["build_sha256"] == expected
)
zone = datetime.timezone(datetime.timedelta(hours=8))


def local(row):
    return (
        datetime.datetime.fromisoformat(row["utc"])
        .astimezone(zone)
        .strftime("%Y-%m-%d %H:%M:%S")
    )


def metrics(rows):
    states = [r["state"] for r in rows]
    return f"{len(rows)} 次采样；总堆 {min(s['heap'] for s in states):,}–{max(s['heap'] for s in states):,} 字节；内部堆 {min(s['internal_heap'] for s in states):,}–{max(s['internal_heap'] for s in states):,} 字节"


report = f"""# 验收记录

最终交付固件：**0.1.1**。实际目标：ESP32-S3-Touch-AMOLED-2.16，ESP32-S3 revision 0.2、8MB PSRAM、16MB Flash。原始16MB Flash完整备份及校验保存在private/，不随发布包分发。

## 稳定性证据与版本边界

| 构建 | 时段（北京时间） | 时长与结果 |
|---|---|---|
| 0.1.0 核心候选 | {local(a[0])} 至 {local(a[-1])} | {a[-1]['elapsed']:.1f} 秒，联网和页面切换通过，无意外重启 |
| 0.1.1 最终修正版 | {local(b[0])} 至 {local(b[-1])} | {b[-1]['elapsed']:.1f} 秒，定向回归后短时稳定性通过 |

候选构建ELF指纹：`{a[0]['state']['build_sha256']}`。
最终构建ELF指纹：`{expected}`。

- 候选：{metrics(a)}。
- 最终修正版：{metrics(b)}。
- 0.1.1补充硬件静音、关闭蓝牙时取消连接、忽略Wi-Fi关闭后的迟到IP事件、启动资源检查，控制消息结构约束，以及字体符号回退。未将候选版本的两小时时长冒充最终二进制的两小时测试。

## 功能验收

- 用户实机确认四角按钮完整可见、触摸准确、提示音正常。
- Wi-Fi配网、失败后恢复原网络；设置跨升级保留。
- Mac加密BLE配对、状态读取、亮度修改与恢复、分片Wi-Fi配置。
- 设备作为BLE中心：发现测试外设的服务/特征/CCCD，读取和订阅模拟22.50°C数据。
- 电池存在、电量、电压、USB与充电状态、PMU芯片温度正常读取。
- 无TF卡挂载失败正常返回，设备继续运行。
- 最终修正版完成静音/音量零值的驱动应用状态回归、Wi-Fi开关恢复、BLE关闭后的连接清理和30次页面切换。

## OTA验收

- 网页签名上传、HTTPS清单与固件下载、分区切换及30秒启动确认。
- 错误凭证403；错误产品、板型、布局、签名和截断镜像400；超大长度413；TCP中断不切换当前运行槽。
- 故意首次启动abort和故意不喂看门狗，两种情况均自动回退并恢复原设置。
- 慢速上传总时限实测约{load('slow-upload-result.json')['elapsed_seconds']:.1f}秒后关闭，原运行构建持续可用。
- 首次USB镜像及OTA镜像均验证RSA签名；未启用硬件Secure Boot、Flash加密或eFuse防降级。

## 实机修复记录

- 将官方BSP的50行PSRAM绘图缓冲改成两个8行内部缓冲，解决Wi-Fi/BLE共存时DMA临时缓冲不足。
- Flash/NVS相关任务保留内部RAM栈；升级工作线程预分配，BLE堆使用受支持的外部内存模式。
- 减少升级准入检查的大快照嵌套栈开销。
- 增加完整签名镜像长度与接收长度比较，防止旧分区尾部补齐截断上传。
- 有界消费被拒请求的剩余数据，改善HTTP错误响应。
- 重新扫描刷新未连接记录，优先保留较强信号的可连接设备；界面按地址选择，正确关联隐私地址连接。

## 未完成的物理验收

- 没有TF卡：真实卡读写、卸载与拔卡兼容性待插卡测试。
- 未执行真正切断全部电源的测试；拔USB且保留电池不等于掉电。
- 电池完整充放电周期、容量/SOC精度、完全冷启动后的充电，以及息屏首次触摸等未做完整人工验收。
- 两个外部BLE设备加一个手机的三连接并发容量尚缺额外实体设备测试；单个外设及手机角色分别实测通过。
- 帧率为UI调度目标，未用仪器测量面板扫描频率；中文动态名称超出当前字库时需要扩字。

主机发布/分区测试12项通过，原生C++策略测试通过；不受信任签名密钥和测试CA均被正式信任链拒绝。

机器记录位于artifacts/；包含访问凭证或原始调试信息的文件位于private/，不进入源代码包。
"""
(root / "docs/VALIDATION.md").write_text(report)
print("Validation report generated")
