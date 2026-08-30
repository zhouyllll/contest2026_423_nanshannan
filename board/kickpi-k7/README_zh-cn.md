# KICKPI-K7（RK3576）开发板 — openvela 适配

[ [English](README.md) | 简体中文 ]

> **状态：骨架已就绪，编译通过，串口参数已由厂商资料确认，等待上板验证**
>
> SoC 层与板级层已建立，`nuttx.bin` 可正常构建（入口 `0x42000000`，
> 带 ARM64 Linux Image 头，可由 U-Boot `booti` 加载）。
> 调试串口的路数、基址、时钟与波特率已由 KICKPI 官方 Armbian 源码中的
> 厂商 U-Boot defconfig 确认，见文末「已确认」。

## 开发板

- **开发板**：KICKPI-K7
- **芯片**：Rockchip **RK3576**（4×Cortex-A72 @2.2GHz + 4×Cortex-A53 @2.0GHz）
- **内存 / 存储**：4·8·16 GB LPDDR / eMMC 16·32·64 GB
- **网络**：双千兆以太网
- **按键**：RESET / POWER / RECOVERY / **MASKROM**

## ★ RK3576 与其它 RK 平台的三点关键差异

移植时最容易踩的三个坑，都是"照抄同厂商其它型号"造成的：

| | RK3399 / RK3568 / RK3588 | **RK3576** |
|---|---|---|
| 中断控制器 | GIC-500 / GIC-600，**GICv3** | GIC-400，**GICv2** |
| DRAM 物理基址 | RK3568 从 `0x0` 起 | **`0x40000000`** |
| 外设地址段 | `0xfxxxxxxx` 高位 | **`0x22000000`–`0x2b060000` 低位** |

三者都属于"填错不报错、上板完全没输出"的静默失效类型。

- **GICv2**：`CONFIG_ARM64_GIC_VERSION=2`。设成 3 的表现是能打印但收不到输入
  （中断永不触发）。参考实现：`boards/arm64/zynq-mpsoc/zcu111`（同为 GIC-400），
  以及 QEMU 配置 `qemu-armv8a:nsh_gicv2`（可在无硬件时先跑熟）。
- **加载地址 `0x42000000`**：来源 U-Boot 主线 `include/configs/rk3576_common.h`
  的 `kernel_addr_r`。照抄 RK3568 的 `0x02000000` 会落进外设区。
- **外设窗口**：`CONFIG_DEVICEIO_BASEADDR = 0x20000000`，大小 192MB，
  需同时覆盖 GIC、UART、CRU、GRF、GPIO，且与 DRAM 不重叠。

## 已适配

| 项 | 状态 | 说明 |
|---|---|---|
| 启动入口 / MMU / 异常向量 | ✅ 编译通过 | `arch/arm64` 通用层 |
| GICv2 中断控制器 | ✅ 编译通过 | `arm64_gicv2.c`，GICD `0x2a701000` / GICC `0x2a702000` |
| Generic Timer | ✅ | ARM 架构标准件，通用层提供 |
| PSCI | ✅ | `arm,psci-1.0`，`smc` 方式 |
| 早期打印（lowputc） | ⏳ 待上板 | DW 8250，等 LSR.THRE 后写 THR |
| 串口控制台 | ⏳ 待上板 | 通用 `uart_16550.c`，DW 8250 与 16550 兼容 |
| GPIO / I2C / SPI | ❌ | 待适配 |
| eMMC / SD | ❌ | 待适配 |
| 以太网 / USB / 显示 | ❌ | 待适配 |
| NPU | ❌ | 寄存器文档未公开 |

启动阶段只跑 A53 簇 core 0（MPIDR `0x0`）单核，未开 SMP。
A72 簇（MPIDR `0x100`–`0x103`）的启动留待后续。

## 关键硬件参数

来源：Linux 主线 `arch/arm64/boot/dts/rockchip/rk3576.dtsi`
与 U-Boot 主线 `include/configs/rk3576_common.h`（均为 Rockchip 官方提交）。

```
GICD              0x2a701000        GICC              0x2a702000
DRAM 基址         0x40000000        kernel_addr_r     0x42000000
SRAM/IRAM         0x3ff80000        CRU               0x27200000
UART0             0x2ad40000  SPI 76  -> IRQ 108   ← 当前假定的调试口
UART2..9          0x2ad50000..0x2adc0000  SPI 78..85
UART10/11         0x2afc0000 / 0x2afd0000  SPI 86/87
UART1             0x27310000
GPIO0             0x27320000        GPIO1..4          0x2ae10000..0x2ae40000
```

全部 UART 为 `snps,dw-apb-uart`，`reg-shift = <2>`、`reg-io-width = <4>`，
对应 `CONFIG_16550_REGINCR=4`、`CONFIG_16550_REGWIDTH=32`。
**这两项漏配会按 8 位间隔访问寄存器，读到全零或垃圾，
表现为串口初始化"成功"但一个字符也不出。**

> GICv2 的中断号 = dtsi 中的 `GIC_SPI` 号 + 32。

## 构建

```bash
cd <openvela 工作区>
source build/envsetup.sh
cd nuttx
./tools/configure.sh -e ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh
make -j$(nproc)
```

产物 `nuttx.bin`，约 304 KB，带 ARM64 Linux Image 头（偏移 `0x38` 处魔数 `ARMd`）。

> 切换构建目标前先 `make distclean`。若切分支后 `distclean` 报
> `chip/Make.defs: No such file or directory`，是上一次配置留下的悬空软链接，
> 先清掉：
> ```bash
> rm -f arch/arm64/src/{chip,board} include/arch/{chip,board} .config Make.defs
> ```

## 烧录与运行

镜像按 Linux Image 格式由 U-Boot 加载：

```
BootROM -> TPL(ddr.bin) -> SPL -> U-Boot -> booti
```

⚠️ **厂商 U-Boot 配置了 `CONFIG_BOOTDELAY=0`**（见 `kickpi-k7-rk3576_defconfig`），
上电不会停在倒计时。要进 U-Boot 命令行，需在上电瞬间持续敲键（通常是
Ctrl-C 或空格），或用 `rkdeveloptool` 进 MASKROM 后重刷。

在 U-Boot 命令行：

```
=> printenv kernel_addr_r          # 确认是 0x42000000，板厂可能改过
=> tftp ${kernel_addr_r} nuttx.bin # 或 fatload / load mmc
=> md ${kernel_addr_r} 10          # ★ 先确认镜像确实落位了再跳
=> booti ${kernel_addr_r} - ${fdt_addr_r}     # 厂商 bootscript 用的也是这条
```

**先用 `md` 确认再 `booti`。** 加载地址错误的表现是完全没有输出且没有报错，
是本阶段最难排查的一类问题。

板上有独立 **MASKROM** 按键，配合 `rkdeveloptool` 可在写坏 eMMC 后恢复。
**建议在写第一行代码之前先完整走一遍恢复流程。**

## 待确认项

### 已确认（依据 KICKPI 官方 Armbian 源码的厂商 U-Boot defconfig）

`patch/u-boot/legacy/u-boot-radxa-rk35xx/defconfig/kickpi-k7-rk3576_defconfig`：

```
CONFIG_DEBUG_UART=y
CONFIG_DEBUG_UART_BASE=0x2ad40000     → UART0
CONFIG_DEBUG_UART_CLOCK=24000000      → 24 MHz
CONFIG_DEBUG_UART_SHIFT=2             → reg-shift=2，即 CONFIG_16550_REGINCR=4
CONFIG_BAUDRATE=1500000
```

| 项 | 取值 |
|---|---|
| 调试串口 | **UART0，`0x2ad40000`，GIC_SPI 76 → IRQ 108** |
| 输入时钟 | **24 MHz** |
| 波特率 | **1500000**（非 115200） |
| 寄存器步进 / 位宽 | **4 / 32** |

### 仍待实测

| # | 项 | 当前取值 | 怎么确认 |
|---|---|---|---|
| 1 | `kernel_addr_r` | `0x42000000`（U-Boot 主线 `rk3576_common.h`） | 上板 `printenv kernel_addr_r`；厂商 U-Boot 可能改过 |
| 2 | DRAM 容量 | 保守取 512MB | 板卡规格；偏小只是用不满，不影响启动 |
| 3 | LED / 按键 GPIO | 未填 | K7 原理图 |

## 参考资料

| 资料 | 链接 |
|------|------|
| 上手指南 / WIKI 文档 | https://doc.kickpi.cn/Products/Beginner-Guide/KICKPI-K7/ |
| 硬件资料 | https://doc.kickpi.cn/Products/Introduction/KICKPI-K7/ |
| 外设接口（CAN 等） | https://doc.kickpi.cn/Products/Peripherals-and-Interfaces/CAN/ |
| K7 RK3576 网盘（百度网盘） | https://pan.baidu.com/s/1cMKQt06pWdxZcsOp1XIvQA?pwd=kpcd （提取码：`kpcd`） |
