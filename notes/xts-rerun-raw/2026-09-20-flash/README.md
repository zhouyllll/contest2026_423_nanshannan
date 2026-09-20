# xTS 1.3.1 烧写测试：PASS（2026-09-20）

原文：按厂商指导文档把版本烧进 Flash，重启进入 nsh。

本项目的烧写入口是 `scripts/flash.sh`（一条命令，不需要碰板子）：
打 FIT → 串口发 `loader` 进下载模式 → **整区备份** → 写入 → **回读比对** →
不一致则回滚 → 复位启动。原始日志 `flash-run.log`：

    FIT bytes: 3576320; LBA 24576..31560 （区域上限 8192 扇区，余 1207）
    current FIT header verified
    FIT write verified. Current region backup: /tmp/k7-amp-fit/flash-20260920T115531Z/lba24576-8192-before.bin
    已复位启动

重启后经网络 NSH 确认系统正常：

    NuttX  0.0.0 ed8f69ad-dirty Sep 20 2026 19:51:56 arm64 kickpi-k7
    Umem  total 63,537,152  used 10,117,120  free 53,420,032

写盘范围由 `amp/layout.sh` 的 `amp_check_write` 把关（只允许写各自分区区间），
这是 2026-09-17 写坏 Linux 内核之后加的护栏。
