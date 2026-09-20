# 1.3.11 UART 文件传输（2026-09-19）

串口 /dev/ttyUSB0，1500000 baud，8N1；主机 lrzsz，板端 rb/sb。
输入为 4096 字节：0..255 重复16次。运行脚本见 capture.py。

结论：未通过双向传输要求。

- 上传：`rb -f /tmp --retry 5`，主机 sb 返回0、Transfer complete。
  板端文件大小4096，后续 md5_test 回读摘要
  `2bcd3c4de20c918e19fab5c36249c70d` 与主机相同，上传功能通过。
- 回传：`sb --retry 5 /tmp/xts-uart-pattern.bin`，主机 rb 返回128、
  Transfer incomplete，没有收到目标文件，无法完成往返逐字节校验。
- download-rx.raw 中 SOH/00/ff 至 NSH 提示符之间的文件名帧只有127字节，
  标准应为133字节；采集到的帧CRC不匹配。只能证明接收记录缺字，
  尚不能区分板端发送、USB串口链路或主机采集层的原因。
- 测试后串口 NSH 恢复正常。重试次数限定5，超时会终止主机程序并发CAN。

events.json 保存两方向退出码、耗时和哈希；*-rx.raw、*-tx.raw 保存协议字节，
*.stderr 保存 lrzsz 原始诊断，*-tail.raw 保存退出后的串口输出。
板端MD5证据见同级 2026-09-19-sweep/run/serial-08.raw。
本轮没有修改驱动、波特率或固件。
