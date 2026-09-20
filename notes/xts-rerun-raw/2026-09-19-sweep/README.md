# xTS 逐项复测（2026-09-19）

按用户要求继续遍历必测项；测试命令不修改断言，提示符返回不等于PASS。
网络NSH为192.168.1.50:2323，同时保存1500000波特率串口原始SYSLOG。
脚本为 capture.py；run/ 下网络raw/text、events、串口raw均原样保存。
serial-markers.jsonl 的偏移用于定位每条命令的串口记录，serial-NN.raw为切片。

运行版本：`NuttX 0.0.0 3b51eaa1-dirty Sep 19 2026 00:21:06 arm64 kickpi-k7`。
主机Git/config仅作环境参考，不等同于固件构建来源。本轮未烧写固件。
主机墙钟有回跳，耗时采用monotonic；不能据墙钟计算24h漂移。

| 项目 | 本轮实测 | 严格结论/限制 |
|---|---|---|
| 1.1.1 内存管理 | 8项测试结束，部分逐项/汇总字节缺失 | 完整日志待补，不能仅据残缺汇总判PASS |
| 1.1.2 调度 | 汇总16/16 | 串口缺字；指南配置差异仍待核对 |
| 1.1.3 系统调用 | 汇总82/82 | 串口缺字；sockettest01另打印errno与预期值不一致，不能把汇总当无异常 |
| 1.1.5 getprime | 148ms | 功能通过 |
| 1.1.6 mm | TEST COMPLETE，无测试失败输出 | 功能通过 |
| 1.1.7 scanf | OK164，FAILED0 | 功能通过 |
| 1.1.8/9 C/Cxx | Hello及构造函数输出完整 | 功能通过 |
| 1.1.10 popen | Calling pclose()，正常返回 | 功能通过；配置差异未补齐 |
| 1.1.11 pipe | Returning success，FIFO/PIPE/redirection完成 | 功能通过；本轮未执行指南重复的rm清理步骤 |
| 1.1.13 cxxtest | vector/map/RTTI/异常测试输出完整 | 功能通过 |
| 1.2.3 RAM占用 | total63565824，初始used12057344/free51508480 | 已统计openvela共享内存堆；AMP另一系统统计待补 |
| 1.2.4 Flash占用 | df仅tmpfs和procfs | 未满足物理Flash/多系统占用统计 |
| 1.3.12 RTC | command not found | 条件不足，未执行规定程序 |
| 1.3.13 Timer | 原命令25.967s；drivertest_oneshot逐项OK | 汇总存在缺字，完整证据待补 |
| 1.3.16 RNG | nist_sts command not found | 条件不足，未完成NIST统计 |
| 1.3.17 Crypto | 八程序均返回；ECDSA生成/签名/验签成功 | 部分CMocka日志缺字；还需确认硬件/软件算法适用范围，暂不整项判严格PASS |

Crypto细节：3DES/AES-CBC/AES-CTR/AES-XTS各汇总1通过；HMAC汇总3通过，
HASH汇总4通过；CRC32四项逐项OK、汇总缺失。网络AES-XTS保留完整向量输出。
所有缺字均保留原样，不通过重构日志补造证据。

当前主机配置缺FS_LARGEFILE、RAMLOG_SYSLOG、MM_KASAN、TESTING_NIST_STS。
必须用可追溯的专用测试镜像补齐配置和无损日志，再补测相关缺口。
旧版80分钟SD压力FAIL仍有效，不能用该次配置片段提交当作复测PASS。

看门狗、重启、第二批存储/内核测试另存 reset/ 和 phase2/；结果见各自events及总清单。
