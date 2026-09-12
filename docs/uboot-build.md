# 搭建自己的 U-Boot（openvela BSP 视角 · RK3576 / KICKPI-K7）

> 状态：**路径 A 已上板验证（2026-09-12）** —— 自编 U-Boot 跑起来，拿到命令行。
> 补丁与复现步骤见 `bsp/uboot/`；本文保留完整方案（路径 B/C 未做）。
> 原始方案文档写于 2026-09-11，所有机制均已在 SDK 源码中核实（引行号）。
> 前置事实：SDK U-Boot 源码完整；KickPi K7 为官方支持板型；官方自带 AMP 配置。

## 0. 现状与目标

**现状（"冒充"启动）**：
```
BootROM → TPL(闭源) → SPL(闭源) → 原厂出厂 U-Boot → boot_android → 读 boot.img
                                                          ↓
                    repack-bootimg.py 把 nuttx.bin 塞进 boot.img kernel 段 → booti
```
- 原厂 U-Boot **静默**（bootdelay=0、控制台关闭）→ 无法干预启动
- 镜像被 Android boot.img 格式锁死（ramdisk/dtb/header 都是壳）

**目标（自己的 U-Boot）**：
1. U-Boot 控制台可用（能 setenv/打断启动/交互调试）
2. 直接启动 openvela nuttx.bin（摆脱 boot.img 壳）
3. （可选）AMP 分核

## 1. 资源清单（SDK 里现成的）

SDK 根目录：`.../rk3576-linux-20260320/rk3576-linux/`

| 资源 | 路径 | 说明 |
|---|---|---|
| U-Boot 源码 | `u-boot/` | Rockchip 2017.09 系 |
| RK3576 板型配置 | `u-boot/configs/rk3576_defconfig` | `CONFIG_TARGET_EVB_RK3576=y` |
| **官方 AMP 配置** | `u-boot/configs/rk3576-amp.config` | 全文 3 行：`CONFIG_AMP=y`、`CONFIG_BASE_DEFCONFIG="rk3576_defconfig"`、`CONFIG_ROCKCHIP_AMP=y` |
| KickPi K7 板型 | `device/rockchip/.chips/rk3576/rockchip_rk3576_kickpi_k7_*_defconfig` | K7/K7C/K7S × Ubuntu/Debian/buildroot |
| 闭源二进制 | `rkbin/` | TPL/DDR/SPL + 打包工具（只引用，不改） |
| 分区表源头 | `device/rockchip/rk3576/parameter.txt` | GPT：uboot/misc/boot/recovery/backup/rootfs |
| 编译入口 | `u-boot/make.sh` | `./make.sh <board>`，工具链用 SDK prebuilts |

## 2. 三个机制（理解配置体系，别混淆）

| 机制 | 管什么 | 改哪里 | 生效 |
|---|---|---|---|
| **Kconfig** | 功能开关（代码进不进固件） | `defconfig` / `.config` 片段 | 编译期 |
| **环境变量** | 运行时行为（bootcmd、加载地址、bootargs） | `include/configs/*.h` 默认环境；运行期 `setenv`+`saveenv` | 运行期可改，默认值编译期 |
| **GPT 分区表** | 存储介质上的分区布局 | `device/rockchip/rk3576/parameter.txt` → 生成 GPT | 烧录时 |

**关键：bootcmd 不在 Kconfig 里，在默认环境里。** 覆写链已核实：

```
include/configs/rockchip-common.h:171-183
    #if defined(CONFIG_AVB_VBMETA_PUBLIC_KEY_VALIDATE)
      RKIMG_BOOTCOMMAND = "boot_android ${devtype} ${devnum};"
    #elif defined(CONFIG_FIT_SIGNATURE)
      RKIMG_BOOTCOMMAND = "boot_fit;"
    #else  ← 当前生效分支
      RKIMG_BOOTCOMMAND = "boot_android ${devtype} ${devnum};" \
                          "boot_fit;" "bootrkp;" "run distro_bootcmd;"
    #endif

include/configs/evb_rk3576.h（板级覆写）
    #undef  CONFIG_BOOTCOMMAND
    #define CONFIG_BOOTCOMMAND  RKIMG_BOOTCOMMAND   ← 板级最终值
```

## 3. 修改清单（文件 × 改什么）

| # | 文件 | 改什么 | 优先级 |
|---|---|---|---|
| 1 | `u-boot/include/configs/evb_rk3576.h` | `CONFIG_BOOTCOMMAND` 覆写（换掉 `RKIMG_BOOTCOMMAND` 或改指向） | ★ 核心 |
| 2 | `u-boot/include/configs/rockchip-common.h` | `RKIMG_BOOTCOMMAND` 宏（boot_android → 你的加载命令）；或新增一个宏供板级引用 | ★ 核心 |
| 3 | `u-boot/include/configs/rk3576_common.h` | `ENV_MEM_LAYOUT_SETTINGS`：`kernel_addr_r=0x40400000`、`fdt_addr_r=0x48300000`、`ramdisk_addr_r=0x4a200000` | ★ 按需 |
| 4 | `u-boot/configs/rk3576_defconfig` | `CONFIG_DEFAULT_DEVICE_TREE="rk3576-evb"`、`CONFIG_BOOTDELAY`、控制台串口 | 按需 |
| 5 | `u-boot/configs/rk3576-amp.config` | AMP：直接叠加（不用改内容） | 可选 |
| 6 | `device/rockchip/rk3576/parameter.txt` | 分区布局（openvela 化时改） | 可选 |
| 7 | `u-boot/board/rockchip/evb_rk3576/` | 板级 C 代码 | 一般不动 |

**不碰**：`rkbin/`（闭源）、SPL 分区（Loader 兜底）、`u-boot/dts/`（U-Boot 自带 dtb，用官方即可）。

## 4. 三条路径（按正规程度递进）

### 路径 A：最小（先拿回控制台）
- 改 `evb_rk3576.h`：`CONFIG_BOOTCOMMAND` 保留 `RKIMG_BOOTCOMMAND` 不变，只开控制台（`CONFIG_BOOTDELAY=2`、确认 debug 串口）
- 结果：**启动行为不变（boot.img 继续工作），但能进 U-Boot 命令行**，可 `setenv` 现场调试
- 风险：最低；收益：获得交互能力

### 路径 B：openvela 化（直接启动 nuttx.bin）
- 改 `rockchip-common.h` 或板级覆写：
  ```
  CONFIG_BOOTCOMMAND:
    "setenv bootargs <openvela bootargs>;"
    "load mmc 0:4 0x40480000 nuttx.bin;"
    "booti 0x40480000 - 0x48300000;"
  ```
- boot 分区直接放**裸 nuttx.bin**（不再用 boot.img 壳）
- nuttx 地址依据：openvela `CONFIG_RAM_START=0x40480000`（nuttx/.config 实测）
- 风险：中；收益：完全掌控引导链、去掉 Android 壳

### 路径 C：AMP（双 OS）
- 编译叠加官方 `rk3576-amp.config`（`CONFIG_AMP=y` + `CONFIG_ROCKCHIP_AMP=y`）
- U-Boot 侧分核逻辑在 `arch/arm/mach-rockchip/amp.c`（SDK 自带，已核实）
- 需 amp.img（从 `~/rk3576-amp/` 现有产物延续）
- 风险：中→大（涉及 SMC/PSCI 分核）；收益：Linux/NuttX 共存

## 5. 编译

```bash
cd <SDK>/u-boot
./make.sh rk3576                  # 基础版（工具链用 SDK prebuilts，自动）
# 或手动：
make rk3576_defconfig              # 或：make rk3576_defconfig rk3576-amp.config
make CROSS_COMPILE=<tc> KCFLAGS="-Wno-error" -j$(nproc)
```
- 已知坑：新版 GCC 把警告当错误 → 必须 `KCFLAGS="-Wno-error"`
- 产物：`u-boot.img`（写 uboot 分区）、`spl/u-boot-spl.bin`（不动）

## 6. 烧录与验证

```bash
# 只动 uboot 分区（SPL 在独立分区，兜底可进下载模式）
rkdeveloptool wlx uboot u-boot.img
rkdeveloptool rd
```

**验证点清单**（串口）：
1. 自己的 U-Boot banner + `Hit any key to stop autoboot`
2. `env print bootcmd` → 看到自己的启动命令
3. 打断进命令行：`mmc part`、`ls mmc 0:4`、`md 0x40480000` 都能用
4. `boot` 正常进 nsh

## 7. 挂死排查（速查）

| 症状 | 定位 | 手段 |
|---|---|---|
| 串口无输出 + USB 无枚举 | BootROM/硬件 | maskrom 恢复 |
| 有 DDR/SPL 打印但卡 | SPL 层 | rkbin 版本/DDR 参数 |
| 有 U-Boot 打印但卡 | U-Boot 层 | 驱动/dtb/AVB 校验 |
| 打印完进不了系统 | bootcmd/镜像 | `env print bootcmd`、`mmc part`、镜像格式 |

**恢复资源**（已备）：`~/rk3576-amp/backup/uboot-orig.img`（出厂 U-Boot）、`~/rk3576-amp/rkbin/rk3576_spl_loader_v1.09.108.bin`（maskrom loader）、`gpt-orig.bin`。

**铁律**：先验证 maskrom 恢复流程再动 bootloader；永远只烧 uboot 分区；出厂备份永远留着。

## 8. 与 openvela 项目衔接

| 项 | 值 | 说明 |
|---|---|---|
| nuttx 运行地址 | `0x40480000`（CONFIG_RAM_START） | bootcmd 加载地址按此定 |
| 现有 flash.sh | 复用 | 打包逻辑换新后改 `FLASH_LBA` 与写盘方式 |
| repack-bootimg.py | 路径 A 继续用；路径 B 退役 | 裸 nuttx.bin 不再需要 boot.img 壳 |
| 时序建议 | 音频收尾后再动 | 动 bootloader 需验证 maskrom，板上调试期避免叠加风险 |

---

**一句话**：SDK 已提供一切（源码/板型/AMP 配置），"搭建自己的 U-Boot" = 改 2 个文件（`evb_rk3576.h` 覆写 bootcmd + `rk3576_common.h` 地址）→ `./make.sh rk3576` → 只烧 uboot 分区 → 串口拿回控制台；要 AMP 就叠加官方 `rk3576-amp.config`。
