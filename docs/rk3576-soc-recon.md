# RK3576 SoC 硬件参数勘察（M1 实现依据）

用 `.claude/skills/soc-hw-recon` 的方法做的第一轮纸面勘察。

**证据等级**：本文所有数值来自 Linux 主线 `arch/arm64/boot/dts/rockchip/rk3576.dtsi`
（torvalds/linux master，2872 行）。主线 dtsi 是 Rockchip 自己提交并长期维护的，
属**强证据**，但仍需上板后按 RECON.md B5 用真实设备树逐项终验。

---

## ★ 关键纠正：RK3576 是 GICv2，不是 GICv3

前期讨论中一度认为 RK3576 用 GICv3。**错误。**主线 dtsi 写得很清楚：

```dts
gic: interrupt-controller@2a701000 {
        compatible = "arm,gic-400";
        reg = <0x0 0x2a701000 0 0x10000>,   /* GICD  分发器          */
              <0x0 0x2a702000 0 0x10000>,   /* GICC  CPU 接口        */
              <0x0 0x2a704000 0 0x10000>,   /* GICH  虚拟化控制      */
              <0x0 0x2a706000 0 0x10000>;   /* GICV  虚拟 CPU 接口   */
        interrupts = <GIC_PPI 9 (GIC_CPU_MASK_SIMPLE(8) | IRQ_TYPE_LEVEL_LOW)>;
};
```

**GIC-400 是 GICv2 实现**（四段 reg = GICD/GICC/GICH/GICV 是 GICv2 的标志；
GICv3 只有 GICD + GICR 两段）。

对照：RK3399 是 GIC-500（GICv3），RK3588 是 GIC-600（GICv3）。
**RK3576 在这一点上和它的前后代都不同**，不能照抄 rk3399 的中断代码。

### 影响

| 项 | 结论 |
|---|---|
| 需要的驱动 | `arch/arm64/src/common/arm64_gicv2.c`（**openvela 已有，不用自己写**） |
| 配置开关 | `CONFIG_ARM64_GIC_VERSION=2` |
| 最佳参考板 | **`boards/arm64/zynq-mpsoc/zcu111`** —— Zynq UltraScale+ 同样是 GIC-400 + GICv2 + UART0 控制台 |
| 上板前可验证 | **`boards/arm64/qemu/qemu-armv8a/configs/nsh_gicv2`** —— QEMU 上的 GICv2 配置，可以先在这上面把 GICv2 路径跑熟 |
| rk3399 还能抄什么 | 启动流程骨架、目录结构、`_boot.c` 的组织方式；**中断部分不能抄** |

**这条是好消息**：GICv2 驱动 openvela 现成，而且有一块真板子（zcu111）和一个
QEMU 配置作为可运行参考，比 GICv3 路径的参考还多。

---

## CPU 拓扑

```dts
cpu-map:  cluster0 = core0..3   compatible = "arm,cortex-a53"   reg = 0x0 0x1 0x2 0x3
          cluster1 = core0..3   compatible = "arm,cortex-a72"   reg = 0x100 0x101 0x102 0x103
```

| 项 | 值 |
|---|---|
| 拓扑 | 4×Cortex-A53 + 4×Cortex-A72，大小核 |
| **启动核** | **A53 簇的 core 0（MPIDR `0x0`）** |
| A53 簇 MPIDR | `0x0` `0x1` `0x2` `0x3` |
| A72 簇 MPIDR | `0x100` `0x101` `0x102` `0x103`（Aff1=1） |

**M1 策略：只跑 A53 core 0 单核，不开 SMP。**
异构多核的簇间启动留到 M2 之后，别在第一步就踩。

> ✅ **openvela `arch/arm64` 原生支持 A53 和 A72**（`ARCH_CORTEX_A53` / `A72`）。
> 这一条比前身项目强很多 —— A311Y2 是 Cortex-A510（ARMv9-A），openvela 无现成支持，
> 那是当时一个未解的风险。**RK3576 把这个风险整个消掉了。**

---

## 定时器与 PSCI

```dts
timer {
        compatible = "arm,armv8-timer";
        interrupts = <GIC_PPI 13 ...>,  /* secure phys   */
                     <GIC_PPI 14 ...>,  /* non-secure phys */
                     <GIC_PPI 11 ...>,  /* virt          */
                     <GIC_PPI 10 ...>;  /* hyp           */
};

psci {
        compatible = "arm,psci-1.0";
        method = "smc";
};
```

| 项 | 值 | 说明 |
|---|---|---|
| 定时器 | ARMv8 Generic Timer，PPI 13/14/11/10 | openvela `arm64_arch_timer.c` 直接可用 |
| PSCI | 1.0，`smc` 方式 | `select ARM64_HAVE_PSCI`，复位/关机/多核启动都走它 |

**含义**：定时器和 PSCI 都是 ARM 架构标准件，openvela 通用层已实现，
SoC 层只需在 Kconfig 里 select，不用写代码。

---

## 串口（M1 的核心）

全部 UART 均为 **`snps,dw-apb-uart`** —— 即 DesignWare 8250，**16550 兼容**。

```dts
uart0: serial@2ad40000 {
        compatible = "rockchip,rk3576-uart", "snps,dw-apb-uart";
        reg = <0x0 0x2ad40000 0x0 0x100>;
        reg-shift = <2>;        /* 寄存器间隔 1<<2 = 4 字节 */
        reg-io-width = <4>;     /* 32 位访问 */
        interrupts = <GIC_SPI 76 IRQ_TYPE_LEVEL_HIGH>;
};
```

### 全部 UART 基址与中断

| UART | 基址 | GIC_SPI | NuttX IRQ（SPI+32） |
|---|---|---|---|
| uart0 | `0x2ad40000` | 76 | 108 |
| uart1 | `0x27310000` | — | — |
| uart2 | `0x2ad50000` | 78 | 110 |
| uart3 | `0x2ad60000` | 79 | 111 |
| uart4 | `0x2ad70000` | 80 | 112 |
| uart5 | `0x2ad80000` | 81 | 113 |
| uart6 | `0x2ad90000` | 82 | 114 |
| uart7 | `0x2ada0000` | 83 | 115 |
| uart8 | `0x2adb0000` | 84 | 116 |
| uart9 | `0x2adc0000` | 85 | 117 |
| uart10 | `0x2afc0000` | 86 | 118 |
| uart11 | `0x2afd0000` | 87 | 119 |

> GICv2 的 SPI 号需 +32 得到硬件中断号。这一步算错的表现是**中断永不触发**，
> 但轮询发送仍然正常 —— 也就是"能打印、但收不到输入"。

### ★ 串口不用从零写

**这是相对前身项目最大的差别。**A311Y2 的 Meson UART 是自有 IP，
寄存器区仅 `0x18` 字节，既不兼容 16550 也不同于 PL011，只能从零写驱动
（见 `archive/a311y2/meson-uart-spec.md`）。

RK3576 的 DW 8250 与 RK3399 是**同一个 IP**，`arch/arm64/src/rk3399/rk3399_serial.c`
就是一个完整的 DW 8250 驱动，寄存器偏移（`THR 0x00 / IER 0x04 / FCR 0x08 /
LCR 0x0c / LSR 0x14 / USR 0x7c`）已把 `reg-shift = 2` 算进去，
**RK3576 的 dtsi 同样是 `reg-shift = <2>`，可原样复用**。

**路线：移植 `rk3399_serial.c`，不走通用 `uart_16550.c`。**
理由与取用边界（该文件是从 Allwinner A64 驱动复制改名而来，含大量残留代码）
见 [`rk3576-port-skeleton.md`](rk3576-port-skeleton.md) 第一节。

早期打印（`_lowputc.S`）仍需自己写几十行汇编，因为它在 MMU 和驱动框架就绪前运行。
抄 `rk3399_lowputc.S` 即可。

> ★ **M1 阶段连时钟和 pinmux 都不用配** —— U-Boot 已经把调试串口初始化好了
> （它自己就在往那个口打印日志）。最小路径是：等 LSR bit5(THRE) 就绪，
> 往 THR 写字节。这把 M1 的依赖压缩到只剩"基址对不对"一项。

---

## 待实测项（纸面拿不到）

| # | 项 | 为什么拿不到 | 怎么拿 |
|---|---|---|---|
| C1 | **K7 的调试串口是哪一路** | 板级决定，不在 SoC dtsi 里 | K7 原理图 / wiki；Rockchip EVB 惯例是 uart0，但**必须确认** |
| C2 | **UART 输入时钟频率** | 由 CRU 配置，dtsi 只给 `<&cru SCLK_UART0>` 句柄 | U-Boot 里 `clk dump`；或按 24 MHz / 48 MHz 试；波特率算错的表现是**输出乱码** |
| C3 | **波特率** | 板级约定 | 引导器用 Rockchip 惯例 **1500000**；但**本项目的 NuttX 控制台已降到 115200** —— 实测 1.5Mbps 下这条 CH340 链路丢 7~16% 的字节（板子发 4096 个 0x00，三次分别收到 3566/3422/3817），115200 下三次都是 4096/4096。日志掉字和 YMODEM 发送方向失败都由此而来 |
| C4 | **内核加载地址** | 由启动链路决定 | U-Boot 环境变量；rk3399/zcu111 都用 `0x02080000` 作参考起点 |
| C5 | **DDR 起始地址与大小** | 板级 | K7 资料；RK3576 DRAM 一般从 `0x40000000` 起 |
| C6 | **外设 MMU 映射窗口** | 需覆盖 GIC(`0x2a70xxxx`) 与 UART(`0x2ad4xxxx`) | 见下 |

### C6 的初步推算

需要同时覆盖：
- GICD `0x2a701000` ~ GICV `0x2a706000 + 0x10000`
- UART0 `0x2ad40000 + 0x100`

一个覆盖 `0x2a000000 .. 0x2b000000` 的 16 MB 窗口即可包住两者。参考写法：

```c
#define CONFIG_DEVICEIO_BASEADDR  0x2a000000
#define CONFIG_DEVICEIO_SIZE      MB(16)
```

> ⚠️ 用 `scripts/check-addr.sh` 校验（已按 rk3576 改写，6 组检查全部通过）。
> **GIC 或 UART 落在映射窗口外的表现是上板完全没输出、也不报错**，
> 是最难查的一类问题（见 `notes/DEBUG-CASES.md`）。

---

## chip.h 参考值（待终验）

对照 `arch/arm64/include/{rk3399,zynq-mpsoc}/chip.h` 的写法：

```c
/* GICv2：GICR_BASE 在 v2 路径下承载的是 GICC（CPU 接口）地址 */
#define CONFIG_GICD_BASE          0x2a701000
#define CONFIG_GICR_BASE          0x2a702000

#define CONFIG_DEVICEIO_BASEADDR  0x2a000000
#define CONFIG_DEVICEIO_SIZE      MB(16)

#define CONFIG_LOAD_BASE          0x02080000   /* ← C4 待验 */
```

参考对照：

| | GICD | GICR/GICC | DEVICEIO | LOAD_BASE |
|---|---|---|---|---|
| rk3399（GICv3） | `0xfee00000` | `0xfef00000` | `0xF8000000` / 128MB | `0x02080000` |
| zcu111（GICv2） | `0xf9010000` | `0xf9020000` | `0xE0000000` / 512MB | `0x02080000` |
| **rk3576（GICv2）** | `0x2a701000` | `0x2a702000` | `0x2a000000` / 16MB | 待验 |

---

## 结论：M1 的纸面阻塞已解除

M1 需要的四类信息，三类已到手：

- ✅ **中断控制器**：GIC-400 / GICv2，地址已知，驱动 openvela 现成
- ✅ **定时器与 PSCI**：ARM 标准件，通用层现成
- ✅ **串口寄存器模型**：16550 兼容，驱动现成，只需配置
- ⏳ **板级具体值**（C1–C5）：需 K7 资料 + 上板实测

**剩下的全是"参考值与实机是否一致"，和前身项目走到的位置相同 ——
区别是这次官方把硬件资料链接直接给了。**


---

## 附：出厂 Android 的 bootargs（板上实测，2026-08-31）

串口接通后从运行中的出厂系统读到（`cat /proc/device-tree/chosen/bootargs`），
这是**第三份独立证据**，与主线 dtsi、厂商 U-Boot defconfig 三方互证。

```
earlycon=uart8250,mmio32,0x2ad40000
androidboot.boot_devices=2a2d0000.ufs,2a330000.mmc,2a310000.mmc
androidboot.fwver=ddr-v1.09-2f85f4b2d4,spl-v1.08,bl31-v1.20,bl32-v1.06,uboot-05/26/2026
storagemedia=emmc  console=ttyFIQ0  androidboot.hardware=rk30board
androidboot.selinux=permissive  kvm-arm.mode=none
```

| 信息 | 值 | 印证了什么 |
|---|---|---|
| 调试串口 | `uart8250, mmio32, 0x2ad40000` | UART0 基址、8250 兼容、**32 位访问**（对应 `CONFIG_16550_REGWIDTH=32`） |
| eMMC | `0x2a330000` = 主线 `sdhci` | M4 存储适配的目标 |
| SD 卡 | `0x2a310000` = 主线 `sdmmc` | 同上 |
| UFS | `0x2a2d0000` = 主线 `ufshc` | 本板未用 |
| 固件栈 | DDR v1.09 / SPL v1.08 / **BL31 v1.20（TF-A）** / **BL32 v1.06（OP-TEE）** / U-Boot 2026-05-26 | 确认 TF-A 在链路中，PSCI 可用 |
| 控制台 | `console=ttyFIQ0` | Rockchip FIQ debugger 占用该串口，**这是串口偶尔丢字符的原因** |

> ⚠️ `console=ttyFIQ0` 说明 Android 侧用的是 Rockchip 的 FIQ 调试器而非标准
> 8250 驱动，两者共用同一个物理串口。这解释了厂商内核 dts 中
> `/delete-node/ chosen;` 且 uart0 未出现在 `&uartN status okay` 列表里的原因。
