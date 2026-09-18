# 1.1.12 MD5 复测

同上一轮 b946208 对应运行镜像，未刷写。原 /etc 未挂载支持普通文件的
文件系统，mkdir 只能创建伪目录，echo 重定向 open failed: 2。
执行 mount -t tmpfs /etc 后，echo xts-md5 > /etc/1.txt 和 cat 回读成功。
执行原命令 md5_test -f /etc/1.txt -c 100，串口原始日志完整包含 100 条
32 位 MD5，全部为 f332259a470403eed260a56443cbcdb0，与主机对
字节串 xts-md5\n 的计算一致。用例预期满足，本轮 PASS。

保留准备阶段 exec failed: 2：源码显示 NSH 先尝试应用 spawn，再回退
内置 shell 命令；该现象与 MD5 摘要不一致不同，后续单独核对。
没有修改 MD5 测试源代码。tmpfs 输入重启后消失，下次复测须重新准备。
日志文件与事件全部原样保存；analysis.json 为对原始串口的计数结果。
