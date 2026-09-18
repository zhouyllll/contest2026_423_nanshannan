# 1.3.3 RAM 读写复测（2026-09-18）

本轮只执行 1.3.3；网络 NSH 为 192.168.1.50:2323。
沿用上一项运行镜像，未重刷、未重启、未修改 RAM 测试程序。

## 实测与判定

测试前 free：total=64045056，used=31197824，free=32847232，
maxfree=32670320 字节。

1. ramtest -w -s 32670320：malloc failed，未进入任何读写阶段。
   后续 free 与测试前一致。这是分配失败，不能判成 RAM 数据错误。
2. 保留 65536 字节余量，执行 ramtest -w -s 32604784：
   Marching ones、Marching zeroes、三组互补 pattern、Address-in-address
   六个阶段输出完整，无 ERROR，正常返回 NSH；主机 monotonic 耗时4.029秒。
3. 测试后 free、used、maxfree 与测试前完全一致，maxused 提升至63814176。

结论：32604784 字节（约31.09 MiB）的32位 RAM 读写功能通过。
严格按指南“读取最大块后使用该 size”口径，仍保留为有步骤偏差、未完全闭环；
不能将减去64 KiB的结果写成最大块原值测试通过。

## 原因核对

apps/testing/mm/ramtest/ramtest.c 的 parse_commandline 使用 malloc(size)，
分配失败时直接打印错误并退出；main 完成测试后 free。
因此 free 报告的最大空闲块并不保证之后新启动任务的 malloc 能分配同样
大小：任务启动期间还会分配栈等资源，分配器也有块管理开销。
这与本轮“原值失败、预留余量成功、测试后空闲恢复”的现象一致，
但本轮没有测量各项开销，不能声称64 KiB就是精确开销。

源码校验错误只打印 ERROR，不改变 main 最终的0返回值，因此本轮依据
全部阶段的完整日志及无 ERROR 判断，不能只以退出码/NSH提示符判 PASS。

raw/txt 和 events.jsonl 原样保留，包括第一次分配失败。
