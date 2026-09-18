# xTS 短测原始记录：2026-09-18

蓝牙排查按用户要求暂停。当前板端为 b946208 对应的 SDIO 诊断镜像，
FIT 哈希见 metadata.json；部署与回读依据见 ../../bt-line-diag-2026-09-18/。
uname 的 3b51eaa1-dirty 是 NuttX 版本标识，不是 BSP Git 提交。
本目录保留网络原始字节、可读副本、命令事件、串口原始字节、配置和工作树差异。
配置为本轮主机快照，不能单独替代运行镜像身份证据。此前测试日志保留原位。

| 用例 | 本轮结果 | 结论 |
|---|---|---|
| 1.1.5 | getprime 完整输出：1230 个素数，149 msec | 本轮步骤通过 |
| 1.3.3 | free 最大空闲块 54179840 字节；1 MiB Marching ones/zeroes、三组 pattern、address 测试无报错 | 1 MiB 功能通过；尚未按最大块大小完整复测，严格项仍待补 |
| 1.3.4 | 按原命令创建 /dev/ram10，串口汇总 PASSED 3 test(s)；逐行仍缺字 | 功能通过，严格原始证据不完整 |
| 1.1.12 | /etc 初始不存在；mkdir 后文件创建仍 open failed: 2；MD5 输出 open fail/cal md5 error | 前置失败，未通过 |
| 1.2.4 | df -h 完整，但仅列 tmpfs/procfs | 缺硬件 Flash 占用说明，未严格通过 |
| 1.3.12 / 1.3.16 | help 未列 cmocka_driver_rtc / nist_sts | 当前镜像缺命令，未测试 |

日志通道观察：网络命令正文完整，但 cmocka/MD5 日志走串口。
short-tests-serial.raw 保存第二次 RAM block、mkdir、echo、cat、MD5 期间
连续串口采集，未补写缺失字符；可见 exec failed: 2。
dmesg 30 秒无提示符，记录为超时，不将空输出视为无错误。
主机墙钟回跳，events.jsonl 中实际时间原样保留；本轮不用于时钟漂移或启动性能。
1.3.14 与 3.1.1 继续暂停；未运行破坏性 Flash 测试、复位或看门狗。

后续优先补齐可靠 syslog 采集，再复跑 MD5 和 CMocka；所有失败、超时也归档。
SHA256SUMS 可校验本目录所有证据文件（不包括自身）。
