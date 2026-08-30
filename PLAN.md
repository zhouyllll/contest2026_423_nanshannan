# 技术方案落地拆解

openvela × Rockchip RK3576（KICKPI-K7）· 新硬件适配赛道

> 本文是 [`README.md`](README.md) 的展开：难度评级、里程碑、知识缺口、风险应对。
> M1 的具体实现参数在 [`docs/rk3576-soc-recon.md`](docs/rk3576-soc-recon.md)。

---

## 0. 三个决定计划形状的事实

### ① openvela 不是 Linux，是基于 Apache NuttX 的 RTOS

Linux 侧的知识分两类：**架构层可迁移**（AArch64 异常等级、MMU、GIC、
Generic Timer、设备树读法、交叉编译与镜像加载），**API 层不可迁移**
（`module_init`、`file_operations`、`platform_driver`、kobject/sysfs 全都没有）。

驱动写法要换成 NuttX 的一套：`uart_dev_s` / `uart_ops_s`、
`ioexpander` / `i2c_master_s` / `spi_dev_s`、`register_driver()`。

### ② 官方已把 KICKPI-K7 挂为待适配目标

`vendor/rockchip/boards/rk3576/kickpi-k7/README_zh-cn.md`（commit `8755815`，
分支 `dev-ai-contest-2026`）明文写「尚未适配 / 本目录不包含任何板级支持代码 /
欢迎提交可用的板级适配 PR」，并给出硬件资料与网盘。

对应赛道「重点鼓励」第二条（全新芯片平台首次适配）。**产出归宿明确：
PR 提交至 `open-vela/vendor_rockchip` 的 `dev-ai-contest-2026` 分支。**

与前身项目的关键差别：`vendor_amlogic` 上游仓库当时**尚未创建**，交付路径不明；
本轮 `vendor_rockchip` 已创建、目录已建好、README 已写好在等人填。

### ③ 赛道的最低达标线是明确的

赛道参赛要求写得很具体：**启动引导 + 至少 UART 控制台输出 + 系统正常运行**。
更完整的驱动矩阵与应用 Demo 属于「评分加分项」。

**这直接决定取舍：M1+M2 是及格线，必须拿下；M3 之后按投入产出排。**

---

## 1. 难度与风险评级

| # | 内容 | 难度 | 风险 | 说明 |
|---|---|---|---|---|
| 1 | 启动链路对接 | 中 | 中 | DDR 由 Rockchip TPL 阶段固件（`MiniLoaderAll.bin`）完成，OS 侧不做；难点是加载地址与入口，需实测 |
| 2 | MMU / 内存映射 | 低 | 低 | 通用层现成，填地址常量；窗口须同时覆盖 GIC 与 UART |
| 3 | **GICv2 接入** | 低 | 低 | ★ **不是 GICv3**。`arm64_gicv2.c` 现成，开关 `CONFIG_ARM64_GIC_VERSION=2`，参考板 zcu111 |
| 4 | PSCI / Generic Timer | 低 | 低 | 通用层现成；dtsi 确认 `arm,psci-1.0` + `arm,armv8-timer` |
| 5 | 核心型号支持 | **低** | **低** | A53 + A72，openvela `arch/arm64` 原生支持。**前身项目的 ARMv9/A510 风险已消除** |
| 6 | **串口** | **低** | **低** | ★ DW 8250 与 RK3399 同 IP，`rk3399_serial.c` 可移植；M1 阶段 U-Boot 已初始化好，只需 lowputc 写 THR |
| 7 | GPIO / I2C / SPI | 中 | 中 | Rockchip 自有 pinctrl/GPIO；I2C 主线标为 `rockchip,rk3399-i2c` 兼容，可参考 |
| 8 | eMMC / SD | 中高 | 中高 | DW MSHC / Rockchip SDHCI，openvela 无现成实现 |
| 9 | **网络（有线 GMAC）** | 中高 | 中 | ★ 板载双千兆，走有线而非 WiFi，难度显著低于 SDIO WiFi |
| 10 | 端侧 AI Demo | 中 | 低 | `apps/mlearning` 的 tflite-micro CPU 推理 + `packages/ai_agent`，不依赖 NPU |
| 11 | 显示 / VOP2 | 高 | 高 | 拓展项，一个竞赛周期内不现实 |
| 12 | NPU（RKNN 6TOPS） | 极高 | 极高 | 寄存器手册不公开、`librknnrt` 闭源 glibc-only，定位为可行性分析 |

### 关于第 6 项：串口这次不是关键路径

前身项目里串口是**最难**的一环 —— Amlogic 的 UART 是自有 IP，寄存器区仅 `0x18` 字节，
既不兼容 16550 也不同于 PL011，openvela 与上游 NuttX 都没有该驱动，只能从零写
（产出见 [`archive/a311y2/meson-uart-spec.md`](archive/a311y2/meson-uart-spec.md)）。

**RK3576 正相反**：全部 UART 为 `snps,dw-apb-uart`，与 RK3399 是同一个 IP。
`arch/arm64/src/rk3399/rk3399_serial.c`（1437 行）就是一个完整的 DW 8250 驱动，
寄存器偏移已把 `reg-shift = 2` 算进去，RK3576 的 dtsi 同样是 2，**可原样复用**。

**路线是移植它，不走通用 `uart_16550.c`** —— `boards/arm64/` 下只有
`vdk/vdk-armv8r` 用通用 16550，rk3399 自己都没走那条路；而且 DW 8250 的
`UART_USR @ 0x7c`（忙状态检测）是标准 16550 没有的寄存器。

> ⚠️ 但不能整个复制：该文件是从 Allwinner A64 驱动复制改名而来，
> 残留着 `A64_CCU_ADDR` 的时钟门控、软复位、pinmux 代码和未改的 `A64_UART3_IRQ`。
> 取寄存器定义与收发逻辑，**丢掉所有 `A64_*` 相关部分**。详见
> [`docs/rk3576-port-skeleton.md`](docs/rk3576-port-skeleton.md)。

★ **M1 阶段更省**：U-Boot 已经把调试串口初始化好了（它自己就在那个口打印日志），
时钟、pinmux、波特率分频在 openvela 接手时全是配好的。最小路径是在
`rk3576_lowputc.S` 里等 LSR bit5(THRE) 就绪、往 THR 写字节，几十行汇编。
**这把 M1 的依赖从四个待实测参数压缩到只剩一个：基址对不对。**

**关键路径因此收敛为「加载地址 + 调试串口基址」两个只能实测的量。**

### 关于第 3 项：GICv2 是本轮唯一的"意外"

主线 dtsi 明确 `compatible = "arm,gic-400"`（GICv2）。
**RK3399 是 GIC-500、RK3588 是 GIC-600，都是 GICv3 —— RK3576 和前后代都不同。**

影响是正面的：openvela 已有 `arm64_gicv2.c`，且参考比 GICv3 路径更多 ——
真板 `zynq-mpsoc/zcu111`（同为 GIC-400）+ QEMU 配置 `qemu-armv8a:nsh_gicv2`。
**后者意味着 GICv2 路径可以在板子到位前就在 QEMU 上跑熟。**

唯一要小心的是：**中断部分不能照抄 rk3399**，以及 GICv2 的 SPI 号需 +32
才是硬件中断号（算错的表现是"能打印但收不到输入"）。

---

## 2. 里程碑

| | 里程碑 | 成功信号 | 权重 | xTS |
|---|---|---|---|---|
| **M0** | 环境与调研 | ✅ 大赛分支构建体系跑通 | 5% | — |
| **M0.5** | SoC 纸面勘察 | ✅ GIC/UART/定时器参数齐备 | 5% | — |
| **M1a** | GICv2 路径预热 | `qemu-armv8a:nsh_gicv2` 跑通并读懂 | 5% | — |
| **M1** | **串口出字符** ★ | 板子打印 banner 与 `nsh>`（**赛道最低达标线**） | **25%** | 1.3.10 |
| M2 | 中断与调度 | `ps` 列出多任务；内核 13 项 cmocka 全过 | 15% | 一.1.1 全部 |
| M3 | GPIO / I2C / SPI | `cmocka_driver_gpio` 等通过 | 10% | 1.3.6、1.3.7 |
| M4 | 存储与文件系统 | 从 eMMC/SD 挂载 | 5% | 1.3.5、1.3.11 |
| M5 | **端侧 AI Demo** | `ai_agent` 或 tflite-micro 跑通（**判定标准对应项**） | 10% | — |
| M6 | 文档与交付 | 适配指南、踩坑记录、AI 日志、Skill、**上游 PR** | 20% | — |
| S1 | 网络 / RTC / 看门狗 / CAN | 拓展加分项 | — | 1.3.12/15/16 |

**M1 与 M5 是两个不能丢的点**：M1 是赛道的技术及格线，
M5 是「基于 openvela 开发」判定标准的落地项。**只有 M1 没有 M5，判定上有风险。**

M6 权重给到 20%，因为赛道加分项明确写了「代码质量高、文档完整，可直接合入
openvela 主线」和「编写了详细的适配指南，方便后续开发者复现」——
而 kickpi-k7 的 README 已经把 PR 归宿指好了，这是投入产出比最高的一块。

---

## 3. 需要补的知识

### 已具备（前两轮中已实际验证）

openvela 构建体系与 `build.sh` 用法、`repo` 多仓库管理、预编译工具链使用、
NuttX 端口的分层结构与 Kconfig 注册点、`arch/arm64` 通用层的边界、
defconfig 机制、QEMU 上的 AArch64 调试、从零写一个串口驱动的完整经验，
以及 [`notes/DEBUG-CASES.md`](notes/DEBUG-CASES.md) 里的排查方法论。

### 必须补

| 缺口 | 为什么 | 优先级 |
|---|---|---|
| **GICv2 与 GICv3 的差异** | RK3576 是 GIC-400。SPI 号偏移、CPU 接口寄存器、初始化序列都与 v3 不同 | **P0** |
| **`uart_16550.c` 的配置项语义** | `REGINCR`/`REGWIDTH`/`ADDRWIDTH`/`CLOCK` 的确切含义，配错不报错 | **P0** |
| **Rockchip 启动链路与烧录** | BootROM → TPL/SPL → U-Boot → OS；**maskrom 恢复必须先学会** | **P0** |
| **AArch64 异常等级与 MMU** | 与前两轮相同，可在 QEMU 上补 | P1 |
| **Rockchip CRU（时钟树）** | UART 时钟频率、后续外设时钟；这是 RK3576 相对 rk3399 最不能抄的部分 | P1 |
| **`packages/ai_agent` 与 `apps/mlearning`** | M5 的落地依托 | P1 |

### 硬件前置条件

- [ ] KICKPI-K7 开发板（赛事发放，**已确认型号**）
- [ ] **USB-TTL 串口线（3.3V）** —— 没有它整个项目无法开始
- [ ] K7 硬件资料：原理图、引脚表、RK3576 TRM（`doc.kickpi.cn` + 百度网盘 `kpcd`）
- [ ] `rkdeveloptool` + maskrom 恢复流程跑通一遍（**在写任何代码之前**）
- [ ] 可运行的出厂固件（★ 用于验证串口物理通路，见 [`RECON.md`](RECON.md) B5）

---

## 4. 目录约定

见 [`README.md`](README.md)「目录布局与工作方式」。要点：

- 本仓库位于 openvela 工作区内 `src/contest/`，这是日志采集的前提
- 对 `nuttx/`、`vendor/` 的改动以 patch 形式留存在 `bsp/`，不入本仓库本体
- 板级代码的权威副本在 `bsp/`，用 `scripts/sync-bsp.sh` 软链进工作区

---

## 5. 风险与应对

| | 风险 | 影响 | 应对 |
|---|---|---|---|
| ~~R1~~ | ~~芯片核心配置未确认~~ | — | **已消除**：板子确认为 KICKPI-K7 / RK3576，主线 dtsi 是权威且长期维护的一手资料 |
| ~~R2~~ | ~~核心型号无现成支持~~ | — | **已消除**：A53 + A72，openvela 原生支持。前身项目的 ARMv9/A510 风险不复存在 |
| ~~R3~~ | ~~串口驱动需从零写~~ | — | **已消除**：DW 8250 = 16550 兼容，驱动现成 |
| ~~R5~~ | ~~vendor 上游仓库未创建~~ | — | **已消除**：`vendor_rockchip` 已创建，`boards/rk3576/kickpi-k7/` 已建好并征集 PR |
| **R4** | **加载地址 / UART 时钟错误导致"没输出也没报错"** | **最难排查的一类问题** | 先用出厂固件验证串口物理通路；U-Boot 里先 `md` 确认镜像落位再 `booti`；`scripts/check-addr.sh` 做多处一致性校验 |
| **R8** | **GICv2 而非 GICv3，且不能抄 rk3399** | 中断不工作，表现为"能打印但收不到输入" | 先在 `qemu-armv8a:nsh_gicv2` 上跑熟；对照真板参考 zcu111；SPI 号记得 +32 |
| **R9** | **Rockchip CRU 时钟树无参考** | UART 波特率算错（输出乱码）；后续外设时钟受阻 | M1 阶段先假定 24 MHz 试；U-Boot `clk dump` 取真值；必要时啃 TRM |
| **R6** | 只完成 BSP 而无 AI 落地 | **可能不满足「基于 openvela 开发」判定** | M5 与 M1 同等重要，不作为可裁剪项 |
| **R7** | 板卡与资料到位时间不可控 | 压缩实际开发窗口 | 板子到位前工作全部前置：M1a 在 QEMU 上跑 GICv2、端口骨架先搭、`uart_16550` 配置项先读懂 |
| **R10** | 赛程时间不足 | 无法达到 M1 | **立刻确认截止时间**，见 [`RECON.md`](RECON.md) B7 |

**前身项目的四条风险（R1/R2/R3/R5）本轮全部消除。**
剩余的技术风险集中在 R4/R8/R9 三条，全部是"参考值与实机是否一致"类型，
只能靠上板验证 —— 和前身项目走到的位置相同，但这次官方把硬件资料直接给了。

**当前最高风险是 R10（赛程时间）与 R7（板卡到位时间）**，都不是技术问题。
