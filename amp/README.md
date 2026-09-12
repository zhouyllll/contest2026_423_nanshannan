# AMP：openvela 跑 A53 簇，Linux 跑 A72 簇

RK3576 是 4×A53 + 4×A72。AMP 就是让两个簇各跑一个**完整的操作系统**，
同时在跑，共享同一片 DDR 和同一个 GIC。

```
        ┌──────────────── RK3576 ────────────────┐
        │  A53 簇 (MPIDR 0x000..0x003)           │
        │     openvela / NuttX   @ 0x4a400000    │
        ├────────────────────────────────────────┤
        │  A72 簇 (MPIDR 0x100..0x103)           │
        │     Linux 6.1          @ 0x40400000    │
        └────────────────────────────────────────┘
              共享：GIC（Linux 拥有 distributor）
                    mailbox group0/3（门铃）
                    0x47800000 4MB（vring + 缓冲池）
```

## 目录

| 路径 | 是什么 |
|---|---|
| `uboot/0004-bootamp-from-ram.patch` | U-Boot：新增 `bootamp <addr>` 命令 |
| `linux/rk3576-kickpi-k7-amp.dts` | Linux 的 DTB：只保留 A72 的四个 cpu 节点 |
| `linux/amp.config` | Linux 的配置增量 |
| `fit/amp.its` | 哪个镜像跑在哪颗核上 —— AMP 的全部契约 |

openvela 那一侧在 `../chip/rk3576/`（mailbox + rptun）和
`../board/kickpi-k7/configs/amp-dual/`。

**谁拥有哪个外设，见 [OWNERSHIP.md](OWNERSHIP.md)** —— AMP 里没有硬件
隔离，两边都去初始化同一个控制器的后果不是报错，而是"某一侧偶发地不
工作"，现场看起来像那一侧自己的 bug。

## 启动是怎么分核的

U-Boot 跑在 MPIDR 0（A53 core0）。`bootamp` 读 FIT，按每个镜像的
`cpu` 属性分核：

1. `linux` 节点 `cpu = <0x100>` → 不是当前核，所以走
   `load_linux_for_nonboot_cpu()`：跑 `boot_fit`/`boot_android` 从 **boot
   分区**把内核和 DTB 读进来，然后 `psci_cpu_on(0x100, entry)`。
2. `loadables` 里的 openvela `cpu = <0x000>` → 正是当前核，于是只记下来。
3. 全部处理完，当前核 `armv8_switch_to_el2()` 跳进 openvela。

**顺序是 Linux 先起**，这不是巧合：GIC 的 distributor 归 Linux，它得先
配完，openvela 那边带着 `CONFIG_ARM64_GICV2_SHARED_DIST=y` 才能接着用。
U-Boot 在 PSCI 之前还会 `setup_sync_bits_for_linux()` 关掉 GICD 并清一个
优先级寄存器，那是 Rockchip 定的交接约定。

两个簇里**剩下的三颗核不在 FIT 里**，各自的 OS 起来以后自己拉
（openvela 走 `arm64_cpustart.c`，Linux 走它自己的 DTB）。

## 为什么不用官方那条路

官方 `amp_cpus_on()` 写死了从**名为 "amp" 的分区**读 FIT，而且在
`rk_board_late_init()` 里无条件执行。板上这张 GPT 是 Android 的，没有
amp 分区，要用就得改分区表 —— 而改分区表和刷 bootloader 一样只能靠
maskrom 恢复。调试阶段不值得为一个还没跑通的引导流程押上它。

`bootamp` 把 FIT 的来源换成内存，分区表一个字节不动；而且它是**手动**
命令，默认 bootcmd 不变，AMP 试炸了下一次上电仍然走原来的路。

## 怎么构建

```bash
# 1. Linux（A72 簇）
K=~/rk3576-amp/kernel-6.1
cp amp/linux/rk3576-kickpi-k7-amp.dts $K/arch/arm64/boot/dts/rockchip/
# 并在该目录的 Makefile 里加一行 dtb-$(CONFIG_ARCH_ROCKCHIP) += rk3576-kickpi-k7-amp.dtb
cd $K && make ARCH=arm64 CROSS_COMPILE=$TC rockchip_linux_defconfig rk3576.config
./scripts/kconfig/merge_config.sh -m -O . .config <本仓>/amp/linux/amp.config
make ARCH=arm64 CROSS_COMPILE=$TC olddefconfig
make ARCH=arm64 CROSS_COMPILE=$TC Image rockchip/rk3576-kickpi-k7-amp.dtb -j

# 2. 打成 boot 分区能认的 Android 镜像
python3 scripts/repack-bootimg.py ~/boot-orig.img $K/arch/arm64/boot/Image out/boot-linux-amp.img \
    --dtb $K/arch/arm64/boot/dts/rockchip/rk3576-kickpi-k7-amp.dtb \
    --cmdline "panic=0 rootwait ro printk.devkmsg=on kvm-arm.mode=none"

# 3. openvela（A53 簇）
cd nuttx
./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp-dual
make -j

# 4. 带 AMP 的 U-Boot
cd ~/rk3576-amp/u-boot
patch -p1 < <本仓>/amp/uboot/0004-bootamp-from-ram.patch
patch -p0 < <本仓>/bsp/uboot/0001-defconfig-bootdelay.patch   # 要有提示符才能敲 bootamp
./make.sh rk3576-amp CROSS_COMPILE=$TC

# 5. AMP FIT
cp nuttx.bin amp/fit/openvela-amp.bin
cd amp/fit && mkimage -f amp.its -E -p 0xe00 amp.itb
```

## 怎么烧、怎么跑

```bash
rkdeveloptool wl  16384 uboot-amp.img        # uboot 分区，4MB
rkdeveloptool wl  51200 boot-linux-amp.img   # boot 分区 ← Linux
rkdeveloptool wl 182272 amp.itb              # 借 recovery 分区放 ← openvela
```

`misc` 分区（LBA 32768）里如果留着 Android 的 BCB，U-Boot 会按它去启动
recovery。清掉它再跑。

上电后串口 **1500000**（U-Boot 与 Android 都是这个速率，NuttX 是 115200），
一直敲 Ctrl-C 打断 autoboot，然后：

```
=> mmc dev 0
=> mmc read 0x60000000 0x2C800 0x1100
=> bootamp 0x60000000
```

成功的判据不是"看到 Linux 的日志"——这一版 Linux 没有控制台（UART0 归
openvela）。判据是 openvela 的 NSH 里：

```
nsh> ampctl status
  握手完成  : 是
  收到门铃  : 1
```

那个门铃只有在 Linux 跑到 `rockchip_rpmsg` 的 probe、建好两个 virtqueue、
填好接收缓冲之后才会发出来 —— 是一个相当靠后、相当可信的存活判据。

## 状态

| 步骤 | 状态 |
|---|---|
| 带 `bootamp` 的 U-Boot | ✅ 上板，`help bootamp` 有输出 |
| Linux 内核 + 只含 A72 的 DTB | ✅ 7.6MB（原 43MB），DTB 里只有 4 个 A72 cpu 节点 |
| openvela @ 0x4a400000 | ✅ |
| AMP FIT | ✅ |
| **U-Boot 把两个簇分给两个 OS** | ✅ **上板验证**（见下） |
| openvela 在 A53 簇跑起来 | 🔶 卡在早期初始化，修掉一处、还在查 |
| rpmsg 握手（端到端） | ⬜ 等 openvela 起来 |

2026-09-12 上板日志：

```
AMP: Brought up cpu[100] with state 0x12, entry 0x40400000 ...OK
I/TC: Secondary CPU 4 switching to normal world boot
AMP: Brought up primary cpu[0, self] with state 0x12, entry 0x4a400000 ...OK
- Ready to Boot Primary CPU / Boot from EL2 / Boot from EL1
- Boot to C runtime for OS Initialize
```

Linux 起在 MPIDR 0x100（A72 簇 core0），openvela 拿到 MPIDR 0（A53 簇
core0）并进到自己的 head.S —— **分核这件事成立了**。

烧写与启动步骤、以及 rkdeveloptool 那个 32MB 静默截断的坑，见
[FLASH.md](FLASH.md)。
