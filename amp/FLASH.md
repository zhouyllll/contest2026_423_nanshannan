# AMP 的烧写与启动

## ★ 一个必须知道的限制：rkdeveloptool 写不到 32MB 以上

`rkdeveloptool wl` 对 **LBA >= 0x10000（32MB）的写是静默丢弃的**：命令
照样报 `Write LBA from file (100%)`，回读却是 0xCC（未写）。

逐项验证过：

| 实验 | 结果 |
|---|---|
| `wl 65528` 写入后回读 | 一致 ✓ |
| `wl 70000` 写入后回读 | 全 0xCC ✗ |
| 换 rkbin 的 `rk3576_spl_loader` 用 `db` 重下再写 | 一样 ✗ |
| `wlx recovery` 走分区名路径 | 一样 ✗ |
| **同一个 LBA 70000 用 U-Boot 的 `mmc write`** | **成功 ✓** |

最后一条把责任划清了：**eMMC 没问题，限制在 USB 通道**（rkdeveloptool
发的是 32 位 LBA，设备侧在 0x10000 处截断）。

这个坑最坏的地方是**它不报错**。第一次往 boot 分区写 46MB 的 Linux
镜像，只有前 7MB 落了地，而工具说 100%。后面所有"为什么起不来"的排查
都建立在一个假前提上。**所有烧写之后都必须回读对比**，这不是谨慎，
是这条通道的必要条件。

## 布局：四段全部落在 32MB 以内

正因为上面这条，AMP 的四个部件都放在够得着的 LBA 上，而且**不使用**
boot 分区的 Android 镜像格式（那样太占地方）：

| LBA | 大小 | 内容 | 原分区 |
|---:|---:|---|---|
| 8192 | 2.2MB | `amp.itb`（AMP FIT，里面是 openvela） | security |
| 12800 | 272KB | `rk3576-kickpi-k7-amp.dtb`（只含 4 个 A72 核） | security |
| 16384 | 4MB | `uboot-amp.img`（带 AMP + bootamp） | uboot |
| 49152 | 7.6MB | `Image-amp`（裸 Linux Image） | vbmeta + boot 头部 |

`Image` 之所以能压到 7.6MB（原来 43MB），见 `linux/amp-minimal.config`
——削掉的都是 AMP 下归 openvela 的外设，既省地方也是资源划分本身。

## 烧

```bash
RK=~/rkdeveloptool/rkdeveloptool
$RK wl  8192 out/amp.itb
$RK wl 12800 out/rk3576-kickpi-k7-amp.dtb
$RK wl 16384 out/uboot-amp.img
$RK wl 49152 out/Image-amp
# 每一段都回读对比，见上
```

## 启

串口 **1500000**（U-Boot、Linux、以及 amp-dual 配置下的 openvela 都是
这个速率 —— 故意统一，否则一条启动流要在两个波特率上分两次抓，
AMP 调试期这一点很要命）。

上电后一直敲 **Ctrl-C** 打断 autoboot（U-Boot 明说了认这个键：
`Hit key to stop autoboot('CTRL+C')`），然后：

```
=> mmc dev 0
=> mmc read 0x40400000 0xC000 0x39A5    # Linux Image ← LBA 49152, 14757 扇区
=> mmc read 0x4f000000 0x3200 0x212     # Linux DTB   ← LBA 12800, 530 扇区
=> mmc read 0x60000000 0x2000 0x10D9    # amp.itb     ← LBA 8192, 4313 扇区
=> setenv amp_linux_cmd 'booti 0x40400000 - 0x4f000000'
=> bootamp 0x60000000
```

`bootamp` 成功不会返回：它最后把当前这颗核送进 openvela。

## 已经跑到哪一步（2026-09-12）

```
AMP: Brought up cpu[100] with state 0x12, entry 0x40400000 ...OK   ← Linux 起在 A72 簇
I/TC: Secondary CPU 4 switching to normal world boot               ← OP-TEE 交给 normal world
AMP: Brought up primary cpu[0, self] with state 0x12, entry 0x4a400000 ...OK
- Ready to Boot Primary CPU
- Boot from EL2
- Boot from EL1
- Boot to C runtime for OS Initialize                              ← openvela 的 head.S
```

**两个 OS 都被拉起来了**，分核这一步成立。openvela 之后卡住，
已定位并修掉一处（`chip.h` 里 `CONFIG_RAMBANK1_ADDR` 写死成
0x40480000，而 amp-dual 链在 0x4a400000 —— MMU 一开，正在执行的代码
就不在映射里）。修完仍然卡在同一行之后，下一轮用
`rk3576_boot.c` 里新加的 `MARK()` 打点定位（它直接写 UART 的 THR，
不经过串口驱动，正是这一段唯一还能出字的手段）。

## 卡住之后怎么恢复

openvela 卡死时板子既没有串口回显也没有 USB 设备，**只能按 RESET**。
复位后默认 bootcmd 会去启动 boot 分区里的 Android 镜像（已被 Image
覆盖），失败后停在 `=>`，正好可以继续调试 —— 这也是保留默认 bootcmd
的好处之一。

要回到"只跑 NuttX"的状态：把 `nsh` 配置编出来，用
`scripts/flash.sh --raw`（nuttx.bin 2.2MB + stub dtb，都在 32MB 以内），
并把 uboot 分区写回 `~/rk3576-amp/backup/uboot-current-working.img`
（那是带 raw-nuttx bootcmd 的那一份）。
