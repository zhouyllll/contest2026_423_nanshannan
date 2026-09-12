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

## ★ 修正（2026-09-12）：Linux 退成纯计算域，摄像头链整条留给 openvela

上一版写的是"摄像头那条链划给 Linux"。**改了**：openvela 独占整条摄像头
硬件链（I2C、sensor MCLK、复位 GPIO、CSI D-PHY、CSI host、CIF），
Linux 完全不碰摄像头硬件，只做 NPU（以及以后可能的 ISP 回读）。

理由是 3A 闭环：`rkaiq` 算出曝光和增益之后，要**按帧通过 I2C 写回
sensor**，而且对帧边界有时序要求（effect-delay 那套账）。如果 sensor 的
I2C 归 openvela，每次 AE 更新都变成一次带 deadline 的核间往返 —— 往一个
本来就对时序敏感的闭环里塞进 rpmsg 的延迟。更麻烦的是 `rkaiq` 期望直接
操作一个 V4L2 subdev，要让它去写一个远端 sensor，得造一层假 subdev 把
`s_ctrl` 转成 rpmsg。**那层 shim 才是真正的工作量。**

把摄像头整条留在 openvela，这个问题整个消失。

### 硬件上这条路成立（已核，不是假设）

RK ISP 有两种输入：

- **online**：CSI host 直接喂给 ISP，不过内存；
- **readback / DMA-in**：ISP 从 DDR 读一帧 raw 进来。

第二种正是这套方案要用的。RK3576 的 ISP 是 **`ISP_V39`（0xa0）**
（`drivers/media/platform/rockchip/isp/hw.c` 的 `rk3576_isp_match_data`）。
判据在 `dmarx.c:1248`：

```c
if (dev->isp_ver >= ISP_V20) {          /* V39 = 0xa0 >= V20 = 0x40 */
        dmarx_init(dev, RKISP_STREAM_RAWRD0);
        dmarx_init(dev, RKISP_STREAM_RAWRD2);
}
if (dev->isp_ver == ISP_V20 || dev->isp_ver == ISP_V30) {
        dmarx_init(dev, RKISP_STREAM_RAWRD1);   /* 这一路 V39 没有 */
}
```

三路 raw-read 通道 V39 有两路，少的 RAWRD1 只影响三帧 HDR。而且
`rkisp_trigger_read_back()` 里有 **V39 专属分支**（调 `rkisp_sditf_sof`、
写 `ISP39_MAIN_SCALE_UPDATE`）—— 说明这条路是**为 V39 维护过的**，不是从
老版本继承下来没人管。

这是驱动层证据，比文档字串强（本项目吃过 dtb 里 `wifi_chip_type`
写错的亏，见 `notes/DEBUG-CASES.md` 案例 25：取参数看驱动实际用的，
认能力别信描述性字串）。但上板之前仍然只算"可用性成立"，不算"跑通"。

### NPU 能吃外来的 carveout，不用改内核

内核 ioctl `rknpu_mem_create` 只能**分配**，`struct rknpu_mem_create`
里没有"从物理地址/dma-buf 导入"的字段。能用的是用户态 API：

```c
/*  rknn_create_mem_from_phys (memory allocated outside)  */
rknn_tensor_mem *rknn_create_mem_from_phys(rknn_context ctx,
                                           uint64_t phys_addr,
                                           void *virt_addr, uint32_t size);
```

它要 phys **和** virt 两个。而 carveout 是 `no-map` 的，Linux 默认没有
虚拟映射。所以 Linux 侧需要**一个几十行的 misc 驱动**（`remap_pfn_range`
把 carveout mmap 出来），或者 `CONFIG_STRICT_DEVMEM=n` 走 `/dev/mem`。
前者干净。这是这套方案在 Linux 侧唯一需要写的内核代码。

### 分三步走

**1. 只做 NPU。** openvela 独占摄像头，取到的图（用现有的
`kickpi_k7_imgproc.c` 做 AWB/LSC 就够）经共享 carveout 交给 NPU。
检测/分类对画质不敏感，不需要 ISP。这一步拿到 AMP 的全部价值，完全躲开
3A，而且 Linux 用户态只要 `librknnrt`，比 `rkaiq + rkisp` 小得多 ——
对 `FLASH.md` 里那个 32MB 烧写限制也友好。

> **再往前切一刀**：第一个里程碑**别带摄像头**。先用一块静态数据过
> carveout 跑一次推理，把"AMP + NPU + carveout + cache 契约"和"摄像头
> 取图"分成两个独立可证的东西。否则第一次不出结果时，候选原因又是五个
> —— 这个项目在音频上已经这么栽过一次。

**2. 再加 ISP 回读，固定曝光。** 只验三个契约，不带控制环。

**3. 确实需要画质时，才做 3A over rpmsg 那层 shim。** 只要固定曝光就
永远不用走到这一步。

### 三个要立的契约

| 契约 | 错了会怎样 | 怎么钉住 |
|---|---|---|
| **raw 的确切排布** | 花屏，**不报错** | 见下 |
| **帧缓冲的 cache 维护** | 偶发撕裂/半新半旧 | clean 在交出去前，invalidate 在读回来前 |
| **buffer 的物理地址进 ISP 的 IOMMU** | ISP 读到别处 | 由 Linux 侧映射 |

**raw 排布不能靠读文档对齐**，因为错了不报错。做法：openvela 往 carveout
里写一个**已知的合成图样**，Linux 读回来 dump 前 N 字节，和主机上算出来的
期望值逐字节比。跑通之后**再故意把打包方式改错一位，确认它会报不一致**
—— 否则这个测试本身不可信。本项目已经被四个"永远返回成功"的假仪器坑过
（`notes/DEBUG-CASES.md`）。

这个契约现在有具体形态了：`rk3576_cif.c` 目前配的是 **UNCOMPACT RAW12**
（每像素占满 16 位，不做位打包 —— 注释里的理由是"显示时取高字节就是
灰度，不需要位解包，少一个环节就少一个可能出错的地方"）。而 ISP 的
RAWRD 入口期望哪种打包，是 stage 2 第一件要核的事。

**cache 那条规则**：vring 那 4MB 可以直接映 Normal-NC（描述符 16 字节，
不缓存的开销无所谓）；帧缓冲不行，必须 cacheable + 显式维护。这一条在
PL330/SAI 上踩过一次 —— 当时缺的是**目的地在 DMA 前也要 clean**，
只做 invalidate 不够。

### 内存布局

```
0x47800000 +  4MB   rpmsg vring + 缓冲池        Normal-NC
0x48400000 + 16MB   OP-TEE                     （谁都不碰）
0x4a400000 + 64MB   openvela
0x4e400000 + 64MB   帧 carveout ← 新增          cacheable + 显式维护
```

全落在 bank1（`0x49400000..0x100000000`）里。

**"一帧多大"取决于分辨率和打包，不要拿一个数当定论**：imx415 是 800 万
像素，满分辨率 UNCOMPACT 是 16.9MB/帧（比压缩 RAW12 还大）；CIF 注释里
提到的 4.2MB 对应 1080p 级别。64MB 够 4 深的环加 NPU 的输入/输出张量。

带宽：CIF 本来就要写一次（sensor→DDR），ISP 回读再加读+写，**总共三趟
不是两趟**。1080p30 约 125MB/s × 3，LPDDR5 双通道 2736MHz 毫无压力；
即使开到满分辨率 30fps（约 500MB/s × 3）也还有余量。

## ★ 按功能链划分，而不是按 IP 块划分

即使按上面这版把摄像头整条留给 openvela，这条原则仍然要记住 —— NPU 那条
链也一样：NPU 本体、它的 IOMMU、它的电源域、它那块 CMA 必须一起划过去，
少划一个就是"NPU 在的时候好、不在的时候坏"这类问题。

"按 IP 块分"最典型的错法就是上一版写的"ISP 给 Linux"：听起来清楚，但
`rkisp` 要出图还需要 i2c4/5/8（配 imx415）、sensor 的 MCLK 分频、复位/
使能 GPIO —— 这些本来都打算给 openvela。正是这一点把方案推到了"Linux 退
成纯计算域"。

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
| CSI D-PHY + CSI host + CIF + i2c4/5/8 + sensor GPIO/MCLK | openvela | 整条摄像头链，见上面的修正 |
| 软件 ISP（AWB / LSC / JPEG） | openvela | `kickpi_k7_imgproc.c`，1235 行，已有 |
| **NPU + 其 IOMMU + 电源域 + CMA** | **Linux** | 连带划，不能只划 NPU 本体 |
| **硬件 ISP（仅回读入口）+ 其 IOMMU** | **Linux** | stage 2 才启用；固定曝光，不做 3A |
| 帧 carveout 0x4e400000 + 64MB | 共用 | openvela 写、Linux 读；cacheable + 显式维护 |
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

Linux 退成纯计算域之后更明显：NPU 的能力全在用户态（`librknnrt`）。
用户态要 rootfs，而**存储归 openvela**。

两条路：

- **initramfs**（选这条）：U-Boot 把 rootfs 一起装进 RAM，Linux 侧完全
  不带存储驱动，也就不存在和 openvela 抢 eMMC 的问题。
- openvela 通过 rpmsg 当文件服务器：灵活，但要自己写一套协议，代价不值。

代价是 Linux 镜像会重新变大（当前 7.6MB 是砍到只剩内核骨架的结果），
那就会撞上 `FLASH.md` 里记的 **rkdeveloptool 32MB 写限制**。届时用
[mk-sdcard.sh](mk-sdcard.sh) 做 SD 启动卡 —— 那个脚本就是为这一步准备的。

不过 stage 1 的 initramfs 可以很小：只要 busybox + `librknnrt` +
一个模型 + 那个 carveout mmap 驱动，压缩后几 MB 量级。Linux 侧不需要
网络、不需要显示、不需要存储 —— 这正是"纯计算域"的好处。

## 当前进度对照

`linux/amp-minimal.config` 关掉的东西和上面这张表是一致的：显示、摄像头、
音频、存储、网络、USB 全部关掉，留下 PSCI / GIC / arch timer / CRU /
mailbox / rpmsg。

**它也关掉了 `MEDIA_SUPPORT` 和 CSI D-PHY** —— 按修正后的划分，这两样
**本来就该关**（摄像头整条归 openvela）。所以这份配置对 **stage 1（只做
NPU）**基本就是最终形态，只需要加回：

    CONFIG_ROCKCHIP_RKNPU=y
    CONFIG_IOMMU_SUPPORT=y / CONFIG_ROCKCHIP_IOMMU=y   （NPU 要）
    CONFIG_DRM=y（最小）                                （rknpu 走 DRM ioctl）

以及那个几十行的 carveout mmap misc 驱动。

到 **stage 2** 才需要把 `MEDIA_SUPPORT` 和 ISP 加回来 —— 但**只加 ISP
本体和 RAWRD 回读入口，不加 CSI PHY / CSI host / sensor 驱动**，因为
输入来自内存而不是 CSI。这一点正是"回读"这条路的价值：Linux 侧要加回的
东西比"整条摄像头链"少得多，也不会和 openvela 抢任何控制器。
