# xTS 内核及驱动短测（2026-09-18）

当前仍为 3990c93 的 RAMLOG 镜像；网络 NSH 192.168.1.50:2323。
所有命令的 raw/txt、事件和合并采集日志均保留。

| 用例 | 实测 | 严格结论 |
|---|---|---|
| 1.1.2 调度 | cmocka_sched_test 16/16 PASSED | 功能通过；指南要求 PSEUDOFS_SOFTLINKS，配置差异待补 |
| 1.1.3 系统调用 | cmocka_syscall_test 82/82 PASSED | 功能通过；指南要求 NET_LOCAL、IOB 128/4、软链接，当前配置不同 |
| 1.1.4 ostest | 运行到双线程 spinlock 后长时间无进展；ps 显示两个 ostest 线程仍 Running/Ready，未打印完成状态 | 超时/卡死，未通过；原始日志保留 |
| 1.1.6 mm | 打印 TEST COMPLETE | PASS |
| 1.1.7 scanftest | Scanf tests done... OK: 164, FAILED: 0 | PASS |
| 1.1.8 C | Hello, World!! | PASS |
| 1.1.9 Cxx | vector/map/RTTI/exception 输出完整 | PASS |
| 1.1.10 popen | Calling pclose() | 功能通过；指南要求 CONFIG_DISABLE_POSIX_TIMERS，配置未满足，严格待补 |
| 1.1.11 pipe | FIFO、PIPE redirection、PIPE test PASSED | PASS |
| 1.1.13 C++ | 异常捕获等预期输出完整 | PASS |
| 1.3.13 oneshot | 25 秒后 drivertest_oneshot 1/1 PASSED | PASS |
| UART 驱动 | drivertest_uart 1/1 PASSED | 设备驱动功能通过，非原清单剩余严格缺口 |

ostest 的终止命令和 ps 快照也保存了；kill 信号没有清除其两个子线程，
因此没有把中断当作通过。后续若继续跑需要先重启板子清理任务。

仍未严格闭环：1.2.1/1.2.2/1.2.4、1.3.3 最大块复测、1.3.5 Flash、
1.3.7 BMI160/I2C-SPI、1.3.12 RTC 配置、1.3.14/3.1.1 长测、1.3.15
watchdog、1.3.16 NIST RNG、2.1.3/2.1.4 启动时间。不要用本批次功能
通过数替代严格 xTS 通过数。
