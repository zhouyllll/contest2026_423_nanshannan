# xTS 内存日志镜像复测（2026-09-18）

增加独立配置片段 xts-logging.config：保留原串口 SYSLOG，同时增加
256 KiB RAMLOG；/dev/kmsg 非阻塞读取，通过网络 NSH dmesg -c 取回后清空。
已构建、备份、写入 FIT 并逐字节回读验证，镜像身份见 metadata.json。
缓冲区有容量上限，不能替代长测持续采集；本轮每组测试后取回，最大单批
启动日志约 22 KiB，未见测试行丢失。串口原始采集仍保留 run/serial.raw。

| 用例 | 命令 / 实测 | 本轮结论 |
|---|---|---|
| 1.1.1 内存管理 | cmocka_mm_test，8 个逐项 OK，PASSED 8 test(s) | PASS |
| 1.1.5 getprime | 完整输出，149 msec | PASS |
| 1.1.12 MD5 | tmpfs 挂到 /etc，准备并回读 1.txt，原命令执行100次；完整日志100个相同摘要 | PASS |
| 1.3.4 RAM 随机读写 | mkrd -m 10 -s 1000 1024，ls /dev/ram10，cmocka_driver_block -m /dev/ram10；3个逐项OK | PASS |

块设备日志 run/20260918T012056Z-dmesg-c.txt；MD5 日志
run/20260918T012058Z-dmesg-c.txt；内存管理完整网络记录见 run/ 下带
monotonic 唯一后缀的文件。未改变测试代码或判断标准。

## 命令准备阶段的 exec failed: 2

apps/nshlib/nsh_parse.c 的 nsh_execute 先调用 nsh_fileapp，再回退 shell
内置命令。nsh_fileapps.c 在 CONFIG_LIBC_EXECFUNCS 下先 posix_spawnp，
没有以 CONFIG_NSH_FILE_APPS 限定；sched/task/task_posixspawn.c 在找不到
可执行程序时打印 ENOENT。故 mount/echo/cat/dmesg 等成功执行仍有此日志。
本轮完整保留该报错；CMocka 三项/八项本身均无失败。这不表示整个启动日志
无异常，也不用于给 1.2.1 启动无异常用例判 PASS。
启动另有 mmcsd CMD1 超时，见第一份 dmesg 原始日志，需独立核实。

## 剩余严格缺口

原16项清单中，1.1.5、1.1.12、1.3.4 已补齐本轮证据，剩13项：
1.2.1、1.2.2、1.2.4、1.3.3、1.3.5、1.3.7、1.3.12、1.3.14、
1.3.15、1.3.16、2.1.3、2.1.4、3.1.1。该数不等于全部35项已严格通过22项，
其他历史结果仍需核原始记录。

RTC 测试编入条件包含 CONFIG_SIG_EVTHREAD，目前缺失；仅补命令不等于
补齐 RTC_ALARM/PERIODIC 等实际能力。RNG 未启用 TESTING_NIST_STS。
RAM 1 MiB 功能结果保留，但按最大空闲块规模复测尚未完成。
Flash 测试尚未选择可安全改写的介质；硬件按键/断电及外设用例仍待执行。
1.3.14/3.1.1 继续按用户要求暂停。主机墙钟不稳定，保留原始时间与
monotonic 耗时，不据此判定时钟漂移/启动性能。

日志脚本默认地址改为实际板端 .50，文件名增加微秒和 monotonic_ns，
避免同秒同命令或主机时钟回跳覆盖原日志。当前已实测新文件名采集。
