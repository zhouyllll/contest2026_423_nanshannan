# SMP 与 AMP

## 一、SMP：四核 Cortex-A53（已完成）

### 核心拓扑

RK3576 是 big.LITTLE，两个簇的 Aff1 不同。出处
`kernel-6.1/arch/arm64/boot/dts/rockchip/rk3576.dtsi` 的 `cpus` 节点：

| 逻辑核 | 型号 | MPIDR | Aff1 | Aff0 |
|---|---|---|---|---|
| cpu_l0..l3 | Cortex-A53 | 0x000 0x001 0x002 0x003 | 0 | 0..3 |
| cpu_b0..b3 | Cortex-A72 | 0x100 0x101 0x102 0x103 | 1 | 0..3 |

八个核 `enable-method = "psci"`；`psci` 节点 `compatible = "arm,psci-1.0"`、
`method = "smc"`。启动核是 A53 簇的 core 0（MPIDR 0x0）。

### 改了什么

NuttX 的 arm64 通用层已经备齐 `arm64_cpustart.c`（PSCI CPU_ON）、
`arm64_smpcall.c`（SGI）、`arm64_gicv2.c`（含 `arm64_gic_secondary_init()`
与支持 SGI 的 `up_trigger_irq()`）。缺的只有**芯片层要提供的两样东西**：

1. `arm64_get_mpid()` / `arm64_get_cpuid()` —— CPU 号与 MPIDR 的互换，
   实现在 `rk3576_boot.c`
2. 两个能力声明（`arch/arm64/Kconfig` 的 `ARCH_CHIP_RK3576`）：
   - `ARCH_HAVE_MULTICPU` —— 否则通用层把 `arm64_get_mpid` 当成宏
   - `ARCH_HAVE_IRQTRIGGER` —— `up_trigger_irq()` 的声明由它守卫，
     缺了它 `arm64_smpcall.c` 报 implicit declaration

配置：`CONFIG_SMP=y`、`CONFIG_SMP_NCPUS=4`。注意 `CONFIG_NCPUS` 是**另一个
独立的 Kconfig 项**（`default SMP_NCPUS if SMP`），per-CPU 变量的数组按它
定尺寸；只改 `SMP_NCPUS` 会得到 `struct tcb_s[1]` 越界的编译错误。

### 只用 A53 簇

当前 4 核全部取自 A53 簇，因此通用层按 Aff0 换算的
`CORE_TO_MPID` / `MPID_TO_CORE` 直接可用，映射是恒等的。

**要扩到 8 核就不能再用默认宏**：通用层的 `MPID_TO_CORE` 只取 Aff0，
A72 簇会算出 0..3，与 A53 簇撞号。届时需要自定义
`cpu = (Aff1 << 2) | Aff0`。

而 A72 簇正是 AMP 阶段准备划给另一个 OS 的那一半，所以现在不占它，
两件事不打架。

★ 更正：AMP 阶段**不需要**改这个映射。`CORE_TO_MPID` 的实现是

```c
__mpidr = GET_MPIDR();                       /* 取当前核的 MPIDR */
__mpidr &= ~(MPIDR_AFFLVL_MASK << AFF0_SHIFT);
__mpidr |= (core << AFF0_SHIFT);             /* 只替换 Aff0 */
```

它保留了**正在运行的这颗核**的 Aff1。所以当 NuttX 的启动核是 A72 簇的
core 0（MPIDR 0x100）时，`arm64_get_mpid(1..3)` 自动得到 0x101..0x103，
`MPID_TO_CORE` 取 Aff0 也仍然得到 0..3 —— 整套逻辑对**任一单簇**都成立，
无需改动。只有想同时用满两个簇（8 核）时才需要自定义映射。

我此前在提交信息里写「扩到 A72 簇需要 Aff1 感知的映射」，那是没读透这个
宏就下的结论，实际只有跨簇才需要。

### 实测

```
ps:   PID 0 CPU0 IDLE Assigned / PID 1..3 CPU1..CPU3 IDLE Running
      affinity 0x0000000f
1.1.2 cmocka_sched_test  16/16（调度到了 CPU1）
1.1.4 ostest             退出码 0
      其中 smp_call_test: Call cwait / Call multi cpu, nowait /
      Call in interrupt, wait / Call multi cpu, wait -> Test success
```

`smp_call_test` 通过意味着跨核 IPI（GICv2 的 SGI9/SGI11）通路是好的。

---

## 二、AMP：Linux + NuttX

> **本节是 2026-09 早期的机制调研，部分结论已被后续工作取代。**
> 传输层（mailbox + rptun/rpmsg）已经写完并上板验证，详见
> **[amp.md](amp.md)**；下面保留调研原文，因为 U-Boot 侧的分析仍然有效。
>
> 一处**决策变更**：本节当时打算让 NuttX 占 A72 簇、Linux 留在 A53 簇
> （理由是"Linux 侧是现成的，不用动"）。实际选了**反过来**：
> openvela 占 A53 簇（含启动核 MPIDR 0），Linux 占 A72 簇。
>
> 换的理由有两条，都比"省事"更重要：
>
> 1. U-Boot 自己就跑在 MPIDR 0 上。让当前这颗核直接进 openvela，
>    是 `rockchip_amp.c` 里最短、不需要核间迁移的那条路；反过来要把
>    U-Boot 的执行流搬到另一个簇，多一步而且没有好处。
> 2. openvela 要做的是实时与产品控制，A53 簇够用且功耗低；Linux 做
>    NPU/ISP/重媒体，正需要 A72。按负载分簇比按"哪边现成"分更合理。
>
> 同届 contest2026_062_PharosTech 在同一块板上按这个拓扑跑通过 4+4，
> 是选它的第三条理由（旁证，不是原因）。


### Rockchip 的 AMP 是怎么做的

不是内核里的某个驱动，而是 **U-Boot 阶段**就把核分掉。
实现在 `u-boot/drivers/cpu/rockchip_amp.c`（533 行），模板
`u-boot/drivers/cpu/amp.its`。

U-Boot 读一个 FIT 格式的 `amp.img`，其中每个镜像声明：

| 属性 | 含义 |
|---|---|
| `type` | `"firmware"`（A 核）或 `"standalone"`（M0 等其它核） |
| `arch` | `"arm64"` / `"arm"` |
| `cpu` | **MPIDR**，例如 `<0x100>` = A72 簇 core 0 |
| `hyp` | 0=EL1/svc，1=EL2/hyp |
| `load` | 加载地址 |
| `boot_on` / `udelay` | 是否启动、启动前延时 |

然后对每个镜像依次调用 `sip_smc_amp_cfg(AMP_PE_STATE/BOOT_ARG01/ARG23)`
配置执行状态与入参，最后 `psci_cpu_on(cpu, entry)` 拉起。

Linux 侧写在 `configurations/conf/linux` 节点里，同样指定 `cpu`，
由 U-Boot 一并拉起；地址仍取 `kernel_addr_r` / `fdt_addr_r` / `ramdisk_addr_r`。

内核 dts 侧配 `/rockchip-amp/amp-cpus`，每个子节点一个 `id`（MPIDR），
告诉 Linux **这些核不归你用**；内存则由 `/memory` 和 `reserved-memory`
划分 —— 文件头注释明确写着「U-Boot 不再负责内存分配/fixup」。

前置条件：`[trust] The AMP feature requires trust support`，即需要 ATF。
我们板上已经有（启动日志里 `INFO: Using opteed sec cpu_context!`）。

### ★ 必须重编 U-Boot

`u-boot/configs/rk3576_defconfig` 里 **AMP 相关配置为零**，AMP 是靠
独立的配置片段打开的：

```
u-boot/configs/rk3576-amp.config
    CONFIG_AMP=y
    CONFIG_BASE_DEFCONFIG="rk3576_defconfig"
    CONFIG_ROCKCHIP_AMP=y
```

所以板厂出厂的 U-Boot 几乎可以确定没有 AMP，**必须重新编译并刷写
bootloader**。这是整件事里唯一有真实风险的一步：刷坏了要进 maskrom 恢复。

### 现状与差距

当前 NuttX 是**冒充 Linux 内核**启动的：`repack-bootimg.py` 把
`nuttx.bin` 塞进原厂 `boot.img`，U-Boot 走正常 `booti` 流程，
按 ARM64 Image 协议搬到 `0x40480000`（DRAM 基址 0x40000000 +
Image 头 text_offset 0x480000），占 64MB。

原厂 `~/boot-orig.img`（64MB）还在，里面是原来的 Linux 内核 + dtb，
eMMC 上的 rootfs 也没动过 —— **Linux 侧是现成的**。

要做成 AMP，NuttX 这边需要改的：

1. **链接地址搬到保留区**（现在 0x40480000 在 Linux 的地盘里），
   并与内核 dts 的 `reserved-memory` 对齐
2. **SMP 改到 A72 簇**：现在我们占的正是 Linux 要用的 A53 簇。
   代码无需改动（见上面的更正），只是启动核由 U-Boot 的 AMP 指定为
   MPIDR 0x100
3. **外设分家**：UART / I2C / GPIO / VOP2 / CIF / GMAC / SD 现在
   全部由 NuttX 独占初始化，AMP 下必须和 Linux 明确划分，
   否则两边同时碰同一个控制器
4. **通信通道**：要让 AMP 有意义，需要 rpmsg/OpenAMP
   （共享内存 + mailbox）。`CONFIG_OPENAMP` 当前未开

### 进度：U-Boot 已编出

已从 SDK 抽出 u-boot + rkbin 到 `~/rk3576-amp/`，用 openvela 自带的
`aarch64-none-elf-` 工具链编译通过（SDK 原本要 gcc-linaro 6.3.1，
新版 GCC 会把若干警告当错误，加 `KCFLAGS="-Wno-error"` 即可）：

```
make rk3576_defconfig rk3576-amp.config CROSS_COMPILE=<tc>
make CROSS_COMPILE=<tc> KCFLAGS="-Wno-error" -j
```

产物 `u-boot.img` 1.45MB。确认 AMP 已链入（`nm` 可见 `amp_cpus_on`、
`arm64_switch_amp_pe`、`sip_smc_amp_cfg`、`os_amp_dispatcher_cpu`）。

★ AMP 逻辑全在 U-Boot 本体（`drivers/cpu/`），**只需替换 `uboot` 分区**，
不必动 loader/TPL。万一 U-Boot 起不来，SPL 仍在，一般还能进下载模式，
风险比换整套 bootloader 小得多。

### 建议的推进顺序

先把风险最大的一步单独验证，再谈功能：

1. 用 SDK 源码编出带 `rk3576-amp.config` 的 U-Boot，**先只刷到 SD 卡
   或先确认 maskrom 恢复流程可用**，不直接覆盖 eMMC
2. 最小 AMP：Linux 跑 A53 簇（原样），NuttX 只占 A72 簇 core 0
   （MPIDR 0x100），单核、只用一个不与 Linux 冲突的串口，
   目标是「两个 OS 同时在跑」
3. 再加 SMP（A72 四核）、外设分家、rpmsg

第 1 步之前不要动 eMMC 上现有的 bootloader。
