# 内置存储与旧 SPIFFS 卷

YA 的 `storage` 分区位于 `0xc20000`，表内大小 `0x3d0000`；Waveshare 原厂在同一地址创建的 SPIFFS 卷为 `0x300000`。SPIFFS magic 包含卷长度，直接按扩大后的分区长度挂载旧卷会返回 `SPIFFS_ERR_NOT_A_FS (-10025)`。旧卷并非空白，不能用挂载失败后自动格式化来处理。

`main/spiffs_compat.cmake` 对固定 ESP-IDF 5.5.5 的 `esp_spiffs.c` 生成构建目录副本，仅将文件系统逻辑长度改为 `main/spiffs_extent.c` 的探测结果。使用同一固定 SPIFFS 的 `SPIFFS_probe_fs`，读取前三个块的 magic，容忍一个中断擦除块；仅接纳块对齐、至少三个块且不大于当前分区的长度。其余情况使用原长度并进入原挂载/错误保留流程。探测只读，分区描述、分区表、擦写地址、A/B 槽和 eFuse 均不变；SDK 源码和依赖锁不修改，补丁锚点失配时构建失败。

原厂旧卷成功挂载后继续使用其原来的 3 MiB 逻辑范围，保留旧文件和尾部未使用区域；不会宣称扩大了旧卷容量。`esp_spiffs_info` 返回扣除 SPIFFS 元数据后的实际可用总容量。新的全空白分区沿用完整分区尺寸初始化。有内容但无法识别、读失败或超出分区的卷保持原样，不自动格式化。TF 卡继续使用独立的 1-bit SDMMC/FATFS 服务。

文件系统挂载、容量读取与文字保存都由现有后台服务执行，遵守 mutation → storage 锁顺序；GUI 只读取状态。蓝牙文字在完整解压校验、写入、同步并读回校验后返回业务成功，失败保留原消息和网页输入。

`tests/test_spiffs_extent.py` 编译同一固定 SPIFFS C 源码，在主机 AddressSanitizer 下验证：3 MiB 旧卷在扩大分区内探测/挂载、旧文件保留、新 UTF-8 文件写入/重挂载、一个中断擦除块、两块损坏、读取失败、超出范围和空白分区不误识别。它不模拟真实 Flash 断电；设备记录见 [VALIDATION.md](VALIDATION.md)。
