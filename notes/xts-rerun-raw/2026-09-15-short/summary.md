# xTS 短项原始记录（2026-09-15）

设备：KICKPI-K7，CH340 `/dev/ttyUSB0`，UART0 NSH 1500000 baud，
以太网 `192.168.1.100` 可 ping。`events.jsonl` 保存每条命令的主机
进入/完成时间、monotonic 耗时、原始文件名及提示符状态；各 `.raw` 保存
实际收到的串口字节，含回显、ANSI 序列和报错。板上镜像 Git 提交尚未从
设备确认，当前工作树 `.config` 不能代替运行镜像证据。主机 WSL 墙钟
发生回跳：第一次 `ramtest`、文件模式 MD5 的 `host_at_done` 小于
`host_at_enter`；这轮不用于启动耗时或时间漂移判定。

| 用例 | 原命令 / 原始文件 | 本轮结果与严格口径 |
|---|---|---|
| 1.1.5 | `getprime`，`20260915T143620Z-getprime.raw` | 找到 1230 个素数，打印 `160 msec`；比第一次采集完整。原版示例为 `getprime took ... msec`，实际镜像输出措辞不同；且串口仍偶发漏字，记功能完成、严格证据待核。 |
| 1.2.4 | `df -h`，`20260915T143629Z-df-h.raw` | 表格缺字/拼接，缺硬件 Flash 分区容量说明；未严格通过。 |
| 1.3.3 | `free`，`20260915T143701Z-free.raw`；`ramtest -w -s 1048576`，`20260915T143524Z-ramtest-w-s-1048576.raw` | 1 MiB RAM 测试到达 NSH，阶段文本缺字；`free` 的最大空闲块字段残缺，原版前置证据不足；未严格通过。 |
| 1.3.4 | `mkrd -m 10 -s 1000 1024`，`20260915T143832Z-mkrd-m-10-s-1000-1024.raw`；`ls /dev/ram10`，`20260915T143841Z-ls-dev-ram10.raw`；`cmocka_driver_block -m /dev/ram10`，`20260915T143848Z-cmocka-driver-block-m-dev-ram10.raw` | RAM 设备存在，块用例汇总 `PASSED 3 test(s)`；逐行 CMocka 文本缺字且 NSH 伴 `exec failed: 2`，记功能通过、严格证据待补。没有测试 eMMC/Flash。 |
| 1.1.12 | `ls /etc`，`20260915T143336Z-ls-etc.raw`；`md5_test -c 100`，`20260915T143945Z-md5-test-c-100.raw`；临时文件 `/tmp/xts1.txt` 的 `md5_test -f ... -c 100 > /tmp/md5.log`，`20260915T145559Z-md5-test-f-tmp-xts1-txt-c-100-tmp-md5-log.raw` | `/etc` 不存在；无 `-f` 被镜像拒绝。临时文件模式输出约 100 条 CPU 日志，但多数摘要末尾串口缺字，`/tmp/md5.log` 为 0 字节；不能证明 100 个完整且一致的值，未严格通过。 |
| 1.3.12 | `cmocka_driver_rtc`，`20260915T143346Z-cmocka-driver-rtc.raw` | `command not found`；当前镜像的 RTC 配置缺原版测试前提（ALARM/PERIODIC/IOCTL 等）；未测试。 |

每次普通 NSH 命令仍会出现
`[CPU0] nxposix_spawn_exec: ERROR: exec failed: 2`。固定短字符串回显
`20260915T143549Z-echo-0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.raw`
完整，较快输出仍有缺字；第二版采集器改为及时读取可用串口字节后，
`df/free` 和 CMocka 文本仍有缺字。不能把不完整的逐行输出修补成 PASS。

两个耗时项 1.3.14、3.1.1 已按用户要求暂停。原起点、6h、12h、
断连、恢复和 18h 记录保留在 `/tmp/k7-xts-longrun/`，没有 24h
终点；12h 普通镜像空跑仅作稳定性观察。

以太网已通，但当前镜像没有 `NSH_TELNET`/`SYSTEM_TELNETD`，主机
22/23/80 无测试入口。当前镜像有 TFTP 客户端，主机未运行 TFTP
服务器；CPU 日志绕过 NSH `>` 重定向，MD5 的 `/tmp/md5.log` 是
0 字节。因此需在下一版测试镜像提供网络 NSH 和可取回的日志通道，
并单独验证网络链路后再用它作严格原始证据。
