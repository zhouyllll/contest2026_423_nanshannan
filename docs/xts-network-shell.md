# K7 xTS 网络 NSH 测试入口

`board/kickpi-k7/configs/amp-dual/defconfig` 在现有串口 NSH 之外启用
NuttX Telnet NSH，监听 TCP 2323。板端 `eth0` 已验证的地址为
`192.168.1.100`，主机为 `192.168.1.101`。`kickpi_k7_appinit.c`
在 `/tmp` tmpfs 挂载成功后追加文件 syslog 通道
`/tmp/xts-syslog.log`，保留原串口 syslog 通道。网络 NSH 可以运行
命令、读取这份文件，解决部分高速 UART 输出缺字导致的原始证据不完整。

新镜像上板后先做三项验证：

1. 串口确认 `nsh>`、`ifconfig eth0`，主机 `ping 192.168.1.100`。
2. 主机连接 `192.168.1.100:2323`，确认获得独立 `nsh>` 会话；
   可用 `python3 scripts/xts-netsh.py 'echo xts-netsh'`，随后运行
   `free`、`df -h`。工具同时保存 TCP 原始字节、去除 Telnet 控制后的
   文本和主机 monotonic 耗时。串口仍须可用作恢复入口。
3. 在网络会话读取 `/tmp/xts-syslog.log`，确认 `[CPU]` 测试日志写入，
   再复测 MD5、块设备等用例并保存文件和 TCP 会话原始输出。

该入口是局域网测试配置，Telnet 没有加密或登录认证；只在受控测试
网络使用。完整 K7/AMP 构建已通过，`nuttx.bin` 为 2,646,016 字节，
SHA-256 为 `33c5d66c0b3b4f0647d40a8e20c4c88b04dccb31d5f292d384b66984a86cda55`。
预打包 FIT 在 `/tmp/k7-netsh-fit/amp.itb`，2,650,624 字节，
SHA-256 为 `76c23608f76a4a12cdc7dcbbceadb7929154e19b7360db9eec643d780db391a4`；
从 LBA 8192 写入时占 5177 扇区，最后 LBA 13368，距 DTB 起点
LBA 14336 仍有 967 扇区。实际端口、文件日志和 xTS 结果均须
在新镜像上板后验证。
