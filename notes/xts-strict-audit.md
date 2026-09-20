# xTS 必测项严格口径复测清单（2026-09-15）

## 2026-09-19 更新

按原文步骤补齐 1.2.1（仅剩 SPL 一行，见表）、1.3.3、1.3.12、1.3.16、2.1.4；3.1.1、1.2.2、2.1.3 于 09-20 完成。启动优化后 2.1.3 冷启动需重测。

## 2026-09-18 更新

已通过完整复测补齐 1.1.5、1.1.12、1.3.4；1.1.1 另复测 8/8。
原16项缺口剩13项，不能由此推算全套通过数。最新结果与全部原始日志见
[内存日志镜像复测](xts-rerun-raw/2026-09-18-ramlog/README.md)。
下文为历史记录；当前板端网络 NSH 是 192.168.1.50:2323，已验证可用。


依据：`docs/refs/openvela_xts_test_cases.md`（社区维护团队 V1.0，2026-05-19）、`docs/xts-checklist.md` 的既有结果、`notes/xts-stability.md` 和 `notes/xts-longrun-start.md`。本表是文档证据审查，不表示已重新上板测试；旧清单的“31 项通过”是内部功能进度，不是严格 xTS 通过数。未列出的 19 项在现有摘要中未发现明显步骤差异，但仍须保存原始输出和镜像版本。

2026-09-15 晚上已暂停 1.3.14/3.1.1 两个耗时项，转跑其他短项。
本轮命令、原始串口和严格结论见
[`xts-rerun-raw/2026-09-15-short/summary.md`](xts-rerun-raw/2026-09-15-short/summary.md)。

2026-09-17 严格复测尝试：最终 FIT（LVGL 256 KiB 修复版）已通过 LBA
8192 写入和回读校验，并在 U-Boot 以 1500000 波特率完整读取 5561 扇区。
启动后板端没有 NSH 提示符，TCP 2323 连接被拒绝；`scripts/xts-short.py`
执行 `getprime` 返回 `No NSH prompt`，115200 和 1500000 串口均无 NSH 输出。
本次所有用例保持“未严格测试/启动阻塞”，不计入 PASS。原始记录见
`/tmp/k7-xts-short/20260916T172032Z-getprime.raw`、
`/tmp/k7-final-boot.raw`。

以太网 `192.168.1.100` 可达，当前镜像尚无网络 NSH/可取回 CPU 日志的
通道；较快串口输出仍缺字，主机 WSL 墙钟出现回跳。这些短项的功能
结果不等于严格 PASS。

本轮长测自 2026-09-15 00:12:02 新加坡时间开始；约 15 小时后，
2026-09-15 15:58:31，串口读取报错并退出，`/dev/ttyUSB0` 随后消失。
原始事件在 `/tmp/k7-xts-longrun/events.jsonl`，串口输出在
`/tmp/k7-xts-longrun/serial.log`。串口恢复后沿原起点只读采到了 18h，
用户随后要求暂停；没有 24 小时终点记录，1.3.14 本轮不能判 PASS。
主机墙钟回跳和串口命令延迟使现有秒级采样也不足以严格判断 ≤2s，
方法修正后须重新设起点并完成整段 24h。

2026-09-15 12:36:41 的 12h 检查点已记录在 `/tmp/k7-xts-longrun/events.jsonl`：
板端 epoch `1789443389`，主机采样时 epoch `1789443393.23`，偏差约 `-4.23 s`；
起点偏差约 `-1.58 s`。只读检查 `/tmp/k7-xts-longrun/serial.log` 未发现
PANIC、ASSERT、watchdog 或 reboot 字样。该观察只是现有镜像的 12h
空跑记录，缺少 KASAN 和 `show_info`，**不按原版 3.1.1 PASS**。
串口断连后无终点采样，现有 6h/12h 采样不能代替 24h 相对漂移判定。

| 用例 | 现有记录 | 严格口径缺口 / 长测后动作 |
|---|---|---|
| 1.1.5 getprime | 旧表标通过，未写实际耗时输出 | 保存原命令输出；缺失则重跑 `getprime`。 |
| 1.1.12 MD5 | `md5_test -c 100` 收到 99 行同值 | 核查是否只是串口漏行；保留完整 100 次结果或重跑并保存日志。 |
| 1.2.1 Reboot 启动异常 | **2026-09-19 按原文 NSH `reboot` ×10**：openvela 与 U-Boot 0 条异常，仅剩厂商 SPL 探 SD 槽 `spl: mmc init failed with error: -123`（10/10） | SPL 这一行需重编 SPL + 重写 idblock 才能去掉，未做；报告中说明。记录：[reboot-logo](xts-rerun-raw/2026-09-19-reboot-logo/README.md) |
| 1.2.2 Cold boot 启动异常 | **2026-09-20**：用户按 RESET 键 5/5 启动成功，平均 2.84s；异常仅厂商 SPL 探 SD 槽一行 | 记录：[coldboot](xts-rerun-raw/2026-09-20-coldboot/README.md) |
| 1.2.4 Flash 占用 | `nuttx.bin` 大小约 1.20 MB | 原文要求设备端 `df -h` 及硬件 Flash 使用情况；补设备输出、分区/镜像说明。 |
| 1.3.3 RAM 读写性能 | **2026-09-19 PASS**：`free` maxfree 53,415,488 → `ramtest -w/-h/-b -s 53349952` 各 6 阶段无错，串口 0 字节 | −64 KB 的原因（ramtest 自身栈/TCB 同堆）见 [ramtest-final](xts-rerun-raw/2026-09-19-ramtest-final/README.md) |
| 1.3.4 RAM 随机读写 | `mkrd -m 10 -s 512 2048`，block 3/3 | 原文要求 `mkrd -m 10 -s 1000 1024` 后在对应 RAM 设备运行 `cmocka_driver_block -m <设备>`；核对实际设备并按原步骤留证。 |
| 1.3.5 Flash 功能 | SD 卡 FAT32 上 `fstest` 20/20 | 原文要求 Flash 设备上的 `cmocka_driver_block -m <设备>`；先确认测试会否破坏数据，再选可安全测试的介质。现结果只作替代功能验证。 |
| 1.3.7 I²C/SPI | RTC/触摸 I²C 正常，SPI 时钟自检 | MPU6050 可作为 I²C 功能复测的实际外设；记录器件、接线、地址、读数及 `cmocka_driver_i2c_spi` 是否可在该器件上运行。原版脚本指定 BMI160，MPU6050 的功能结果作为替代证据，正式等效性需社区确认。另一块开发板并非这条用例的前提。 |
| 1.3.12 RTC | **2026-09-19 PASS**：`cmocka_driver_rtc` 连跑 3 次均 api/alarm/periodic 3/3 OK | 驱动补了 wdog 闹钟/周期唤醒（HYM8563 硬件闹钟只到分钟）；记录 [rtc](xts-rerun-raw/2026-09-19-rtc/README.md) |
| 1.3.14 时间一致性 | 原采集器约 15h 串口断连，续采到 18h 后暂停，无 24h 终点 | 修正主机时钟及采样时间戳后重跑完整 24h，核对四次采样、终点与起点的相对漂移（≤2 秒）；设时命令伴随 `exec failed: 2`，需查明是否违反“无报错”。 |
| 1.3.15 Watchdog | 触发复位与恢复，四个 cmocka 子项未跑完 | 原文要求依次 `-r 0/1/2/3`、前三项 assert/栈及复位原因、末项 PASS；先解决测试和驱动行为，再逐项留证。 |
| 1.3.16 RNG | **2026-09-19 PASS**：原文步骤 `nist_sts 400000` 读 `/dev/urandom/`（指向硬件 RNG），15 类 188 行，最小 P=0.002042，0 处 `*` | 修了 stdio 流数上限 16（lib_fopen）；记录 [nist-sts](xts-rerun-raw/2026-09-19-nist-sts/README.md) |
| 2.1.3 Cold Boot 时间 | **2026-09-20 PASS**：用户上下电 10 次，平均 **2.844 s**（2.806–2.883），阈值 ≤4 s | 起点取上电后第一行 DDR 日志（拔电产生的 0x00 噪声不算）；记录：[coldboot](xts-rerun-raw/2026-09-20-coldboot/README.md) |
| 2.1.4 Reboot 时间 | **2026-09-19 PASS**：NSH `reboot` ×10，`NuttShell (NSH)` 平均 **2.76 s**（2.74–2.77），10/10 | 记录：[reboot-logo](xts-rerun-raw/2026-09-19-reboot-logo/README.md)。当日优化：U-Boot 倒计时 0.2s、触摸/TF/摄像头后台初始化（4.47→2.68s，加 logo 2.76s） |
| 3.1.1 12h 待机 | **2026-09-20 PASS**：KASAN + showinfo 镜像静置 12h，0 次重启、0 行异常关键词、showinfo 713 条心跳无空档、堆无泄漏 | 记录：[kasan-12h](xts-rerun-raw/2026-09-19-kasan-12h/README.md) |

复测顺序：先归档本轮 12h/24h 日志与漂移结论；再做无需重刷的短项；之后在适当镜像上跑 BMI160、RNG、RTC、Watchdog 等配置相关项；最后用 KASAN 镜像重跑 3.1.1。每条记录应有镜像 Git 提交、配置、命令、原始串口输出、时间和结论。不得把替代测试直接标成原版 PASS。
