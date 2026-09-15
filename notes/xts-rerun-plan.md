# xTS 严格口径重跑执行单（2026-09-15）

本单列出证据审查中尚不能按原版判 PASS 的 16 项。逐项原文见
`docs/refs/openvela_xts_test_cases.md`，已有证据与缺口见
`notes/xts-strict-audit.md`。所有复测均记录：镜像 Git 提交、defconfig 与
运行时 `.config`、设备/外设、起止时间、原命令、完整串口原始输出及结论。
串口 `/dev/ttyUSB0` 当前未枚举；恢复 USB 转发并确认 `nsh>` 后才能上板。
不要把本轮 12h 普通镜像空跑或 15h 中断的 24h 长测写作严格 PASS。

## 先恢复与留证

1. 恢复 CH341 的 USB 转发，核对 `/dev/ttyUSB0` 和 1500000 baud 的 NSH。
   先只读 `date`、`help`、`ls /dev`、`free`，保存输出和启动日志；确认板子
   没有因转发断连而重启。若端口编号变化，记录新的设备节点。
2. 归档 `/tmp/k7-xts-longrun/events.jsonl` 与 `serial.log`。起点、6h、12h
   检查点和断连事件均保留；1.3.14 本轮结论为“中断、无 24h 终点”。
3. 为每项建立 `notes/xts-rerun-raw/<用例号>/`，保存 `meta.md` 与原始
   `serial.log`。若一次串口会话测多项，保留整段会话并在 `meta.md` 标注
   起止偏移，避免截取输出造成证据遗漏。`meta.md` 写命令、设备名、
   镜像提交、配置、时间、预期/实际、PASS/FAIL/替代验证/待测。

## 可在当前镜像先做的短项

| 用例 | 原版步骤与需要保留的证据 |
|---|---|
| 1.1.5 | `getprime`；完整输出须出现 `getprime took ... msec`。 |
| 1.1.12 | 准备 `/etc/1.txt` 后 `md5_test -f /etc/1.txt -c 100`；保存全部 100 个一致的值。若 `/etc` 只读，先记录无法创建原因并准备测试镜像。 |
| 1.2.4 | `df -h`，附 eMMC/Flash 总容量、分区和镜像占用说明；AMP 双 OS 如分别提交须保留两侧数据。 |
| 1.3.3 | `free` 记录最大空闲块，再选不超过该块的大小运行 `ramtest -w -s <size>`；保存所有阶段与 PASS。 |
| 1.3.4 | `ls /dev`，`mkrd -m 10 -s 1000 1024`，确认创建的 RAM 设备名后 `cmocka_driver_block -m <RAM 设备>`；保存两条完整结果。 |
| 1.3.12 | `cmocka_driver_rtc`；保存完整 PASS 与 RTC 设备配置。 |

## 需专门器件、配置或镜像

| 用例 | 复测准备与动作 |
|---|---|
| 1.3.7 | 先以 MPU6050 做 I²C 实物功能复测：核对 3.3V、GND、SCL、SDA，找到实际 I²C 总线。当前镜像已编入 `i2c` 工具，可用 `i2c get -b <总线> -a 68 -r 75` 读 WHO_AM_I；若 AD0 拉高，地址改为 `69`。随后记录加速度寄存器读数和完整原始输出。指南的 `cmocka_driver_i2c_spi` 使用 BMI160 驱动；不能仅换接 MPU6050 就把该命令判 PASS。若要按原版脚本跑，准备 BMI160，单次只启用 I²C 或 SPI 一种总线，保存脚本输出；MPU6050 的结果单列为替代验证，并请社区确认是否接受等效器件。 |
| 1.3.16 | 编译启用 `CONFIG_TESTING_NIST_STS=y` 的镜像，准备 `/tmp/experiments/AlgorithmTesting/` 原文要求的目录；运行 `nist_sts 400000`，交互输入 `0`、`/dev/urandom/`、`1`、`0`、`10`、`1`；保存终端记录和 `finalAnalysisReport.txt`，逐项检查 P-Value > 0.0001 与 `/*` 标记。当前镜像未启用该程序。 |
| 3.1.1 | 另编译 `CONFIG_MM_KASAN=y` 且含 `show_info`/`LOW_RESOURCE_TEST=y` 的镜像，确认设备未配网；连续静置 12h，保存全程串口日志，并检查报错、crash、重启。普通镜像在 2026-09-15 已完成 12h 空跑，只作稳定性观察。 |

## 需重启、长时间运行或确认介质安全

| 用例 | 复测准备与动作 |
|---|---|
| 1.2.1 | 从 NSH 执行 `reboot`，保存从命令到 `NuttShell (NSH)` 的完整启动日志及报错检查；与 2.1.4 的 10 次复测共用原始日志。 |
| 1.2.2 | 设备 reset 按键与完整启动日志；注明这是 reset 测试，不能混写为拔电冷启动。 |
| 1.3.5 | 确认待测 Flash 设备、分区和数据可恢复后，才运行 `cmocka_driver_block -m <Flash 设备>`；测试可能改写介质，先核对测试实现与可用测试分区。原 SD 卡 `fstest` 仅作辅助结果。 |
| 1.3.14 | 恢复稳定串口后重新设时，与 PC 同步；保存设时输出与 `date` 回读，每隔 6h 采样共四次，满 24h 后保存终点并计算相对漂移（门槛 ≤2s）。若 `date -s` 仍伴 `exec failed: 2`，先查明报错再判定。可与 3.1.1 同期运行，但必须使用符合 3.1.1 前提的镜像。 |
| 1.3.15 | 核对 watchdog 驱动/测试修复和 reset-cause 记录后，按顺序执行 `cmocka_driver_watchdog -r 0/1/2/3`；0/1/2 各保存 assert、堆栈、重启及 `BOARDIOC_RESETCAUSE_SYS_RWDT`，3 保存正常喂狗 PASS。每次触发重启前保留串口日志。 |
| 2.1.3 | 真正上下电 cold boot 10 次，主机串口时间戳记录启动开始到 `NuttShell (NSH)`；计算平均值，门槛 ≤4000ms。旧的下载模式复位 5 次平均 4795ms，步骤和数值均未过关。 |
| 2.1.4 | NSH `reboot` 10 次，逐次保存从命令到 `NuttShell (NSH)` 的时间戳；平均门槛 ≤6000ms，与 1.2.1 共用日志。 |

执行顺序：先当前镜像短项和 MPU6050 替代 I²C 验证，再确认 Flash 测试
介质安全并做需要重启的项；随后测试 RNG 镜像；最后在 KASAN 镜像上同期
启动 3.1.1 与 1.3.14。Watchdog 的复位测试不能插入长测窗口。
