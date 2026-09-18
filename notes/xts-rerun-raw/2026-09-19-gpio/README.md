# 1.3.6 GPIO：2026-09-19 实测失败

用户确认40针排针第5/7脚短接。源码映射：gpio2=GPIO4_A4输出，
gpio3=GPIO4_A6输入。本轮未更改驱动、未刷机。
运行 uname：NuttX 3b51eaa1-dirty Sep 19 2026 00:21:06 arm64 kickpi-k7。
主机BSP当前HEAD为666cffb；不能仅凭HEAD认定运行镜像包含全部工作树变化。

## 结果

设置gpio3输入；gpio2设输出并写0，自身Verify=1，gpio3输入=1。
gpio2写1，自身Verify=1，gpio3输入=1。低电平传递失败。

执行 cmocka_driver_gpio -i /dev/gpio3 -o /dev/gpio2 -l -p 0 -r 1。
网络正文记录 output bool、loop 的0/1不一致；首次rw字符也出现48/49不一致。
为补抓串口syslog重复同一命令，串口汇总2失败：bool和loop；rw和interrupt
标记OK。rw使用随机值，第二次通过不能否定第一次低电平失败。
串口日志仍有缺字，所有收到的原始字节原样保留，不拼接补写。

重要：原测试interrupt仅assert poll返回值>=0，超时0也会标OK；
因此本轮没有有效中断通过证据。不能以2/4汇总认为中断已正常。
源码参数为-i/-o，与指南示例-a/-b不同，本轮记录了实际参数。

## 当前边界

测试判FAIL；尚未证明是接线、输出方向、复用、外部电平或驱动读回问题。
不能用简单复测全高来宣布修复。需检查GPIO4_A4/A6寄存器和实际电平。
当前主机配置SYSLOG_DEVPATH=/dev/ttyS1，已不同于先前RAMLOG测试镜像；
dmesg采集15秒未返回（日志保留），随后改串口旁路采集。
当前工作树含他人音频/SAI改动，仅保存状态，不提交这些改动。

本目录保存低高电平测试、CMocka两次执行、串口原始数据及主机配置快照。
SD块测试复测仍按用户要求暂停，未在本轮启动。
