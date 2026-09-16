# K7 xTS 网络 NSH 测试入口

## 当前状态：2026-09-16，尚未通过板端验证

首版 FIT 已写入并逐字节回读一致，但板上 U-Boot 的
`AMP_FIT_CNT=0x1400` 只读取 5120 扇区，首版需要 5177 扇区。
无参数 `bootamp` 因缺少尾部而报 SHA-256 校验失败。这是独立于
DTB 分区边界的限制，刷写前检查分区边界并不能发现它。

在同一次 U-Boot 会话补读完整 FIT 后，地址形式 `bootamp 0x60000000`
进入 NuttX 初始化，随后串口无回显、TCP 2323 拒绝连接。
115200 与 1500000 都未得到回显；当前 AMP 配置实际选择 UART0 为
控制台，速率 1500000，UART1 为 115200。能 ping 通目标地址不足以
证明控制台正常。

修订版暂时移除启动期文件 syslog 注册及配置以隔离故障，保留 Telnet
NSH。Kconfig 和完整编译已通过，尚未上板；文件 syslog 是否为根因
仍待对照验证。当前代码不再创建 `/tmp/xts-syslog.log`。

修订版 FIT：`/tmp/k7-netsh-no-file-fit/amp.itb`，2,646,528 字节，
5169 扇区，SHA-256：
`97a8cd17fb236fa227cd5eea8238b2fba864879ca25ede923cc6953bd117652b`。
它也超过旧 bootamp 上限，不能直接按旧无参数命令启动。

原 AMP 区域备份：
`/tmp/k7-netsh-fit/flash-20260915T155742Z/lba8192-6144-before.bin`，
SHA-256：`d6c17c9dfd166732e384c2aba7e48d9b0e11287930b635917b515f96401bdcd0`。
镜像及备份在主机 `/tmp`，使用前必须确认仍存在且哈希一致。

本次成功部署沿用 `scripts/flash.sh` 的 USB 转接方式：U-Boot 执行
`rockusb 0 mmc 0`，从 WSL 调用 Windows `usbipd.exe list` 及
`attach --wsl --busid`，确认 `rkdeveloptool ld` 为 Loader 后写入。
BUSID 会变化。Maskrom 下载流程未建立稳定的 Loader。

刷修订版时须显式设置
`K7_NETSH_FIT=/tmp/k7-netsh-no-file-fit/amp.itb`，脚本默认仍指向首版。
地址形式的 bootamp 要求 Linux、DTB、FIT 及 AMP 启动环境均已准备；
仅加载 FIT 不足以在一次全新的 U-Boot 会话启动双系统。

待完成：修订版上板、NSH 与 2323 验证、修正 bootamp 读取上限，
再开展 xTS 短测与蓝牙、Agent 的板端验证。

## 首版设计与构建记录（历史，不代表当前功能已通过）

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

部署前可运行 `bash scripts/flash-xts-netsh-fit.sh --prepare` 检查 FIT
大小、头部和分区边界。板子进入 RockUSB Loader 且设备转接到 WSL 后，
脚本会先读取 LBA 8192 起的完整 6144 扇区到 `/tmp` 备份，检查原 FIT
头部，再写入新 FIT 并逐字节回读；写入或回读失败时恢复备份并再次回读。
脚本完成后保持 Loader 状态，重启和 `bootamp` 需单独进行。当前 WSL
没有串口或 RockUSB 设备，新镜像尚未写入板子。
