# openvela × Rockchip RK3576（KICKPI-K7）BSP 适配

**队伍**：`contest2026_423_nanshannan`　**赛道**：新硬件适配

在 KICKPI-K7（Rockchip RK3576）上完成 openvela 从 0 到 1 的首次适配 ——
启动引导、GICv2 中断、串口控制台、基础外设驱动，以及端侧 AI 能力 Demo。

## 作品简介

openvela 官方已在大赛分支上把 KICKPI-K7 挂为**待适配目标**：
`vendor_rockchip` 的 `boards/rk3576/kickpi-k7/` 目录下只有一份适配指引 README，
明文标注「尚未适配、本目录不包含任何板级支持代码、欢迎提交可用的板级适配 PR」。
本作品即针对该目标完成 openvela 在 RK3576 平台上的首次适配。

| 核查项 | 结论 |
|---|---|
| openvela `arch/arm64/src/` | 有 a64 / rk3399 / imx8 / imx9 / vdk / zynq-mpsoc，**无 rk3576** |
| 上游 Apache NuttX | 有 rk3399、rk3588，**无 rk3576** |
| `vendor_rockchip/boards/rk3576/kickpi-k7/` | **仅有 README，无任何板级代码** |

## 当前状态

**SoC 层与板级层已建立，编译通过，等待开发板到位验证。**

```
nuttx.bin        311296 字节
Entry point      0x42000000
Image 魔数       ARMd（U-Boot booti 可直接加载）
中断控制器       arm64_gicv2.o（GICv3 未编入）
地址一致性       scripts/check-addr.sh 六组检查全过
```

已完成：启动入口 / MMU / 异常向量、GICv2 接入、Generic Timer、PSCI、
早期打印与 16550 串口控制台配置、SoC 参数勘察、xTS 必测项清单、
地址一致性自动校验、Skill 沉淀。

唯一阻塞：**KICKPI-K7 的调试串口是哪一路 UART**，需原理图确认（见 `RECON.md` B1）。

## ★ RK3576 的三个坑（都属于"填错不报错、上板无输出"）

| | RK3399 / RK3568 / RK3588 | **RK3576** |
|---|---|---|
| 中断控制器 | GIC-500 / GIC-600，**GICv3** | GIC-400，**GICv2** |
| DRAM 物理基址 | RK3568 从 `0x0` 起 | **`0x40000000`** |
| 外设地址段 | `0xfxxxxxxx` 高位 | **`0x22000000`–`0x2b060000` 低位** |

依据：Linux 主线 `rk3576.dtsi` 与 U-Boot 主线 `include/configs/rk3576_common.h`
（`CFG_SYS_SDRAM_BASE 0x40000000`、`kernel_addr_r=0x42000000`），均为 Rockchip 官方提交。

## 运行方式

```bash
# 1. 拉取工程
repo init -u https://github.com/open-vela/contest2026_423_nanshannan \
  -b dev-ai-contest-2026 -m contest2026_423_nanshannan.xml
repo sync -c -j8

# 2. 编译
cd contest2026_423_nanshannan && source scripts/env.sh
cd ../nuttx
./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/nsh
make -j$(nproc)

# 3. 校验地址一致性
../contest2026_423_nanshannan/scripts/check-addr.sh
```

板级代码在本仓 `board/kickpi-k7/`，由 manifest 的 `<linkfile>` 映射到
`vendor/openvela/boards/contest2026_423_board`，**生产仓库零改动**。

烧录与上板步骤见 [`board/kickpi-k7/README_zh-cn.md`](board/kickpi-k7/README_zh-cn.md)。

## 目录说明

| 路径 | 内容 |
|---|---|
| `board/kickpi-k7/` | ★ **板级适配代码**（结构与最终 PR 目标 `vendor_rockchip/boards/rk3576/kickpi-k7/` 一一对应） |
| `chip/rk3576/` | ★ **芯片层**（原 `arch/arm64/src/rk3576`）。按《新平台适配指南》「不得修改核心代码」搬到树外，由 `CONFIG_ARCH_CHIP_CUSTOM_DIR` 加载 |
| `bsp/nuttx-drivers.patch` | 要提给上游 NuttX 的**通用驱动**：新增 `drivers/timers/hym8563.c`（I2C RTC）+ 修 `drivers/input/ft5x06.c` |
| `bsp/nuttx-history.patch` | 完整提交序列，开发过程记录（含芯片层搬迁前的历史） |
| `bsp/vendor-rockchip-toplevel/` | 获奖后 PR 到 `vendor_rockchip` 所需的顶层 Kconfig / Make.defs / Makefile |
| `docs/rk3576-soc-recon.md` | SoC 硬件参数勘察（GIC / CPU / 定时器 / 12 路 UART 全表） |
| `docs/xts-checklist.md` | xTS 必测项清单 = 开发路线图与验收标准 |
| `docs/m1a-gicv2-qemu.md` | M1a：QEMU 上 GICv2 路径验证与驱动分析 |
| `docs/refs/` | 官方文档离线副本 |
| `notes/DEBUG-CASES.md` | 踩坑记录（现象—排查—根因—修复—验证） |
| `scripts/` | `env.sh` / `check-addr.sh` / `sync-bsp.sh` / `logs.sh` / `run-qemu.sh` |
| `.claude/skills/soc-hw-recon` | ★ Skill 沉淀：新 SoC 硬件参数勘察方法论 |
| `logs/` | AI Coding 日志 |
| `archive/` | 前身项目（Amlogic A311Y2）产物，不参与构建 |
| `PLAN.md` / `RECON.md` / `VelaPort-K7-项目描述.md` | 方案、待核实问题、★ 报名正文 |

## 提交路径

| 代码 | 比赛期间 | 获奖后 |
|---|---|---|
| 板级 `board/kickpi-k7/` | fork 本仓 → PR → 自行 review 合入 | PR 到 `vendor_rockchip` 的 `dev-ai-contest-2026` |
| 通用驱动 `bsp/nuttx-drivers.patch` | fork `open-vela/nuttx` → PR 到 `dev-ai-contest-2026`，组委会 review | — |
| 芯片层 `chip/rk3576/` | 随本仓提交；获奖后 PR 到 `vendor_rockchip` 的 `chips/rk3576/` | — |
