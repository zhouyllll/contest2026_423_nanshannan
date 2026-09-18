# 1.3.5 Flash 功能：执行前核查（2026-09-18）

状态：尚未执行写入测试，待确认可覆盖介质。

网络 NSH 已采集 ls /dev 和 mount。当前存储节点只有 /dev/mmcsd1；
board/kickpi-k7/src/kickpi_k7_appinit.c 的 SD 初始化段将 SD 控制器
注册为 minor 1，并确认该节点。当前未挂载 SD 文件系统，不能由此
推断卡中无数据。未发现已提供的独立可覆盖测试分区。

原命令：cmocka_driver_block -m /dev/mmcsd1。
apps/testing/drivers/drivertest/drivertest_block.c 核查结果：
- stress 从扇区0开始逐扇区随机写入、读回并比较 CRC，覆盖总扇区数的约95%。
- single_write 和 cache_write 也从扇区0写入。
- teardown 只关闭设备，不恢复原始数据。
- 参数只接受 -m 设备名，没有偏移或长度限制。

因此直接对整卡执行会破坏分区表和原文件系统。尚未获得确认可擦除卡，
也尚未建立经校验的整卡备份，不应直接执行。不能用先前 RAM 盘3/3通过
替代此 Flash 用例结果。使用 SD 作为平台 Flash 测试介质的适用性需明确记录。

本目录保留本轮只读原始日志。未刷写、未格式化、未运行 block 写测试。
