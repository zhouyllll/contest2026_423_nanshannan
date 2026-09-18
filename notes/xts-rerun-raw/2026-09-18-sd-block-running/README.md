# 1.3.5 SD 卡块测试：运行中

用户确认启动镜像在 eMMC 后授权清空 SD 卡。执行前核对板级源码：
eMMC 注册 minor 0，DWMMC SD 控制器注册 minor 1；/dev/mmcsd1 存在，
mount 未显示该卡的挂载。本次唯一写测试目标为 /dev/mmcsd1。

实际命令：cmocka_driver_block -m /dev/mmcsd1。
当前结果：日志进入 drivertest_block_stress，无结束汇总，不计 PASS。
测试从扇区0起覆盖约95%容量；single/cache子项也会写起始扇区。
SD 卡原分区表、文件应按可能被覆盖处理；未经整卡备份，不能承诺恢复。
本轮没有写 eMMC，也没有重新格式化 SD 卡。

持续采集目录：/tmp/k7-xts-135-run。
主测试命令的网络 raw 文件持续写入，test-capture.log 在命令结束时输出结果。
monitor.log 每30秒收集一轮 dmesg -c，并保存相应网络 raw/txt/events。
采集器最多运行24小时；采集超时不会停止板端测试，更不表示测试通过。
monitor.py 为实际后台采集脚本。后台进程启动后仍须检查日志是否持续更新。

本目录为开始测试时的证据快照。结束后须追加完整日志、结果和校验清单，
并检查三个子项都完成；SD 卡结果不能未经说明替代板载 eMMC 的验证。
