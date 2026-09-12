# AMP 资源划分：谁拥有什么

AMP 里两个 OS 平级、互信、共享同一片 DDR 和同一个 GIC。**没有硬件隔离**
——任何一个资源如果两边都去初始化，后果不是报错，而是"某一边偶发地
不工作"，而且现场看起来像那一侧自己的 bug。所以划分必须提前写清楚，
不能等出问题再对。

## 总原则

**openvela 是产品，默认拥有一切。Linux 只拿它非拿不可的那几条链。**

Linux 出现在这套系统里只有一个理由：**NPU 与 ISP 的厂商软件栈只有
Linux 版**（`librknnrt`、`rkaiq`），移植代价远大于让它跑在另一个簇上。
除此之外 Linux 不该拥有任何东西。

## ★ 按功能链划分，不按 IP 块划分

这是最容易做错的一步。"ISP 给 Linux"听起来清楚，但 `rkisp` 要出图，
需要的远不止 ISP 的寄存器：

| ISP 这条链上的东西 | 为什么绕不过 |
|---|---|
| MIPI CSI D-PHY、CSI host | 数据通路本体 |
| **I2C 到 sensor**（imx415 @0x37，在 i2c4 / i2c5 / i2c8 上） | 不配 sensor 就没有数据 |
| **sensor 的 MCLK**（CRU 分频）与复位/使能 GPIO | sensor 不上电不出时钟 |
| ISP 的 IOMMU | ISP 写内存要过它 |

所以 **i2c4 / i2c5 / i2c8、那几个 GPIO、那个 MCLK 分频一起划给 Linux**，
openvela 不去碰。反过来 NPU 那条链干净得多（NPU + IOMMU + 电源域 +
一块 CMA），基本自洽。

## 划分表

| 资源 | 所有者 | 备注 |
|---|---|---|
| A53 簇 MPIDR 0x000..0x003 | openvela | Linux 的 DTB 里**删掉**这四个 cpu 节点 |
| A72 簇 MPIDR 0x100..0x103 | Linux | openvela 的 SMP 只覆盖 A53 |
| UART0（IRQ 108） | openvela | 调试口（CH340）。Linux 侧 `status = "disabled"` |
| UART5_M0（40pin 21/23 脚） | Linux | 要看 Linux 日志时接第二个 USB-TTL；当前未启用 |
| VOP2 / DSI / HDMI / 背光 | openvela | LVGL 在这一侧 |
| 触摸（FT5x06 @ i2c) | openvela | |
| SAI + ES8388 + 耳机检测 | openvela | |
| SD / eMMC | openvela | 启动期由 U-Boot 使用 |
| GMAC0 / GMAC1 | openvela | |
| USB | openvela | |
| SPI / PWM / SARADC / 看门狗 / RTC | openvela | |
| **CSI PHY + CSI host + ISP + 其 IOMMU** | **Linux** | 连带 i2c4/5/8 与 sensor 的 GPIO/MCLK |
| **NPU + 其 IOMMU + 电源域** | **Linux** | |
| GPU | 谁都不要 | 两边都不初始化 |
| mailbox group0 / group3 | 共用 | openvela 发 B2A、收 A2B（IRQ 174）；见 [../docs/amp.md](../docs/amp.md) |
| 0x47800000 + 4MB | 共用 | vring + rpmsg 缓冲池，映成 Normal-NonCacheable |

## 三个分不掉的东西

这几个全芯片只有一份，只能定"谁说了算"，不能切开。

### 1. CRU（时钟）

**好消息**：RK3576 的 CRU 寄存器是高 16 位写使能的
（`(1 << (bit+16)) | (val << bit)`），**单次写天然原子**，不需要读改写。
所以两个 OS 各自开关自己那条外设的门控位是安全的，不会互相覆盖。

**不安全的是改 PLL 频率**。PLL 是多个外设共用的上游，一边改频率，另一边
的波特率/采样率/像素时钟全变。官方答案是走 **SCMI**：由 ATF 持有 PLL，
两侧都通过 SCMI 请求。当前阶段的做法是**约定谁都不改 PLL**，各自只用
U-Boot 留下的频率再分频。

### 2. PMU（电源域）

一边把某个域关掉，另一边正在用的外设就死了，而且是静默死。这也是
`amp-minimal.config` 里关掉 `CPU_FREQ` / `CPU_IDLE` / `SUSPEND` /
`PM_DEVFREQ` 的**真正原因** —— 不是为了省体积，是为了不让 Linux 的
电源管理去动 openvela 还在用的域。

### 3. GIC distributor

已经踩过，记在这里：Linux 的 `gic_dist_init()` 会把**所有** SPI 的
`ITARGETSR` 都写成它自己那颗核 —— 它不知道有一部分中断属于另一个 OS。
于是 openvela 即使把 `ICDISER` 的使能位置上了，中断也只送到 A72，
本核一个都收不到。

症状极具欺骗性：**所有轮询型驱动（I2C / SPI / RTC）一切正常，日志一路
往下打；到第一个依赖中断的驱动（本项目是 SDHCI）就无声挂住。** 看起来
像那个驱动的问题，实际上整机一个中断都没有。

解法在 `bsp/upstream/gicv2-shared-distributor.patch`：openvela 在
`up_enable_irq()` 里先按**字节**写 `ITARGETSR`/`IPRIORITYR` 把这条 SPI
认领到本核，再置使能位。GICv2 规定这两个寄存器按字节寻址（一个 IRQ
一个字节），所以写一个字节碰不到邻居，共享 distributor 下也安全。
本核在 GIC 里的 mask 从 IRQ 0..31 的 `ITARGETSR` 读出来（那一段 banked
且只读，读回的就是读它的那颗核），不靠 MPIDR 猜 —— 跨簇时 MPIDR 和
GIC 的 CPU 接口编号没有保证的对应关系。

只有 `ICDICFR`（触发方式，每个 IRQ 2 位、4 个挤一个字节）真的需要读改写，
那一个保持只校验、不一致返回 `-EPERM`。

## 一个连带后果：Linux 的 rootfs 从哪来

Linux 只做 ISP/NPU 听起来更简单，但这两样**能力全在用户态**
（`librknnrt`、`rkaiq`）。用户态要 rootfs，而**存储归 openvela**。

两条路：

- **initramfs**（选这条）：U-Boot 把 rootfs 一起装进 RAM，Linux 侧完全
  不带存储驱动，也就不存在和 openvela 抢 eMMC 的问题。
- openvela 通过 rpmsg 当文件服务器：灵活，但要自己写一套协议，代价不值。

代价是 Linux 镜像会重新变大（当前 7.6MB 是砍到只剩内核骨架的结果），
那就会撞上 `FLASH.md` 里记的 **rkdeveloptool 32MB 写限制**。届时用
[mk-sdcard.sh](mk-sdcard.sh) 做 SD 启动卡 —— 那个脚本就是为这一步准备的。

## 当前进度对照

`linux/amp-minimal.config` 关掉的东西和上面这张表是一致的：显示、摄像头、
音频、存储、网络、USB 全部关掉，留下 PSCI / GIC / arch timer / CRU /
mailbox / rpmsg。

**但它也关掉了 `MEDIA_SUPPORT` 和 CSI D-PHY** —— 那是 ISP 要用的。所以
这份配置是**打通阶段**的形态，不是最终形态：等 ISP/NPU 上场，要另开一份
配置把摄像头那条链加回来，同时把 i2c4/5/8 从 openvela 侧摘掉。
