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

**openvela 已在 KICKPI-K7 上启动并运行，核心外设与端侧 AI 能力均已上板验证。**

```
nuttx.bin        1,986,560 字节
Entry point      0x42000000
Image 魔数       ARMd（U-Boot booti 可直接加载）
CPU              4× Cortex-A53 SMP（A72 簇未启用）
中断控制器       GIC-400 / GICv2
```

### 已上板跑通

| 子系统 | 验证方式 |
|---|---|
| 启动 / MMU / 异常向量 / GICv2 / Generic Timer / PSCI | 稳定启动至 `nsh>` |
| **SMP**（4× Cortex-A53） | `ps` 四核在线、`cmocka_sched_test` 16/16、`ostest` 退出 0、`smp_call_test` |
| 串口（16550，12 路 UART 已勘察） | 控制台 1.5 Mbaud（与厂商 U-Boot 一致） |
| **千兆网口**（GMAC + Maxio MAE0621A PHY） | 双向 ping 0% 丢包、1000M 全双工、TFTP 收发文件 |
| eMMC / TF 卡（dwcmshc + dw-mshc） | 挂载读写；`cmocka_driver_block` |
| GPIO / I2C / SPI / RNG / 看门狗 / RTC(HYM8563) | 对应 cmocka 驱动用例 |
| 显示（VOP2 + MIPI DSI + 触摸 FT5x06） | 送屏出图 |
| 音频（SAI） | `cmocka_driver_audio` |
| **摄像头**（IMX415 → CSI D-PHY → CIF） | 取实帧，`INTSTAT=0x300`，动态范围正常 |
| **图像处理**（去马赛克 GBRG + 灰世界 AWB + 面积平均缩放 + JPEG） | 1932×1096 与 1280×720 实拍图，白平衡 G/R=1.02 |
| **V4L2**（`/dev/video0`） | `v4l2cap` 复刻 ai_agent 相机工具的调用序列，出 1280×720 JPEG |
| **ai_agent** | 接 MiMo 后端（OpenAI 兼容），TLS 握手 + 真实对话，`status=ok` |
| HDMI | 控制器探测：`CORE_ID` 读出 `"HQTX"`，电源域/时钟到位（尚未出图） |

### 进行中

| 项 | 状态 |
|---|---|
| HDMI 输出 | 硬件参数与 PHY 方案已定（见 `docs/` 与提交记录），待接显示器做链路 |
| WiFi / 蓝牙（AP6256 = BCM4345C5） | 硬件与固件出处已确认，`dw-mshc` 已支持双实例，待接 SDIO 实例 |
| M.2 SSD（PCIe + NVMe） | 勘察完成，见 `docs/pcie-nvme.md`；NuttX 无 NVMe 驱动，需移植 |
| Linux + NuttX AMP | U-Boot 侧未通，见 `docs/smp-amp.md` |

### 沉淀

88 次提交；`notes/DEBUG-CASES.md` 20 例排查记录（现象—歧路—根因—修复—教训），
其中多例是**上游缺陷**并已归档为可提交的补丁；1 个可复用 Skill。

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
repo init -u https://github.com/open-vela/manifests -b dev-ai-contest-2026 \
  -m contest2026_423_nanshannan.xml
repo sync -c -j8

# 2. ★ 把本仓接进构建树（必须，见下方说明）
./contest2026_423_nanshannan/scripts/setup-workspace.sh

# 3. 编译
source contest2026_423_nanshannan/scripts/env.sh
cd nuttx
cp ../contest2026_423_nanshannan/board/kickpi-k7/configs/nsh/defconfig .config
make olddefconfig && make olddefconfig      # 需要两遍，见下
make -j$(nproc)                             # 产出 nuttx.bin

# 4. 烧录（全自动，不需要碰板子）
cd ../contest2026_423_nanshannan && ./scripts/flash.sh
```

**第 2 步为什么必须。** 大赛 manifest 只为每队映射了三个**模板**目录
（`app/hello_app`、`quickapp/hello_quickapp`、`board/contest_board`），
而本作品的代码在 `board/kickpi-k7/`、`chip/rk3576/`、`app/{cam,v4l2cap,hdmi,
spi_selftest}` 下。manifest 在组委会仓里改不了，所以这些映射、以及公共仓
的适配补丁，由 `setup-workspace.sh` 建立（幂等，可重复执行）。跳过这一步
的表现是"配置里找不到板子"，而原因离现象很远。

**`olddefconfig` 为什么要跑两遍。** 第一遍解析 `select`/`depends on` 之后
才会显现出新的可见符号，第二遍才把它们的值定下来。只跑一遍会静默丢掉
一部分配置 —— 编译不报错，只是功能不存在。

烧录原理与恢复手段见 [`board/kickpi-k7/README_zh-cn.md`](board/kickpi-k7/README_zh-cn.md)。

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
| `docs/smp-amp.md` | SMP 启用过程与 Linux+NuttX AMP 方案 |
| `docs/ai-agent-setup.md` | ai_agent 后端配置与上板验证 |
| `docs/pcie-nvme.md` | M.2 SSD（PCIe + NVMe）勘察与移植方案 |
| `bsp/upstream/` | ★ **上游侧缺陷修复与适配补丁**（TFTP 空指针、agent 墙钟计时等），各自独立提 PR |
| `app/` | 板级验证程序：`cam`（摄像头/图像）、`v4l2cap`（V4L2 全链路）、`hdmi`、`spi_selftest` |
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
