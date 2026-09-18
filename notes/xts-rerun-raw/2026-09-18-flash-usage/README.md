# 1.2.4 Flash 资源占用复测：2026-09-18

本轮仅执行 1.2.4，结论：未严格通过，缺硬件 Flash 资源统计。

## 实测

- 串口 /dev/ttyUSB0 在 1500000 波特率返回 NSH。
- 网络 NSH 192.168.1.50:2323 恢复，网络日志含完整 df -h。
- uname：NuttX 0.0.0 3b51eaa1-dirty Sep 18 2026 09:18:30 arm64 kickpi-k7。
- df -h：/data tmpfs 20K、使用 11K；/tests 和 /tmp tmpfs 各512B；/proc procfs。
- mount 确认四个挂载均非 Flash 文件系统。
- /dev 只有 mmcsd1 存储节点，本轮未识别其介质类型或容量，不能称为 eMMC。
- ps 中已无上次 ostest 残留任务。该事实不等于 ostest 已通过。

## 缺口

指南要求硬件 Flash 占用，AMP 情况还需提供各侧使用情况。
tmpfs 为内存文件系统，其 df 数值不能用于 Flash 统计。
主机 amp/layout.sh 记载 FIT 位于 eMMC LBA24576，占用区域8192扇区，
Linux 从49152起，DTB从14336起；这些是主机部署布局，不能单独替代
板端总容量、当前分区和各镜像有效大小的读回证据。Linux侧统计尚缺。
下一步需要可靠的 eMMC 只读几何/分区查询和 Linux 侧入口后补测。
没有格式化、挂载写盘、刷机或重启。

## 日志注意

第一次串口 uname 已实际返回提示符，随后异步 Agent 日志使旧采集脚本
末尾提示符判定超时；这是采集器误判，不能据此声称 NSH 不存在。
该原始串口日志保留。后续网络采样均返回提示符。
Windows 当时只枚举 CH340（4-9，Attached），未枚举 RockUSB；
本轮实际处于 NuttX 运行状态，而不是可访问的 Loader。

network/ 中保存每条网络命令原始字节、可读文本和事件时间。
probe/ 保存串口探测及失败采样；失败与部分输出均保留。
