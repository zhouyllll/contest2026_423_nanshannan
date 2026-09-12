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

## ★ 第二个坑：借来当中转站的分区，U-Boot 每次启动都会写

把 `amp-solo.itb`（A/B 对照用）放在 LBA 30000，写完回读校验**通过**。
后来同一段再读，尾部 393 字节变成了 `\x00AB0\x01...` —— Android A/B 的
**bootctrl BCB**。

原因：那份 FIT 跨过了 LBA 32768，也就是 `misc` 分区的起点，而 U-Boot
每次走 Android 启动路径都会往 `misc` 写 BCB（日志里的
`ANDROID: reboot reason` / `Vboot=0, AVB images, AVB verify`）。

**写入和当时的校验都是对的，是后来被覆盖的。** 所以"烧完校验一次"
不够 —— 借分区当中转站时，必须借 U-Boot **从不写**的那一段。

已知会被 U-Boot 写的：`misc`（每次启动）。实测安全的：`security`
（8192 起）、`trust`（24576 起，但**不要跨过 32768**）。

这个坑的排查路径也值得记：先在主机上验证 FIT 文件自洽（数据段哈希 ==
裸镜像哈希）→ 再用 `md.b` 看 RAM 头 16 字节（对）→ 再用 U-Boot 的
`cmp.b` 比两次 `mmc read`（一致，读是可靠的）→ 最后 `cmp.b` 比"已知好的
那份"和"这份"的数据段，差异点 0x159A00 折算回 LBA 32772，正好压在 misc
上。**每一步都换一个已知正确的对照物**，而不是反复怀疑同一个环节。

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

## ★ 第三个坑：Linux 的 earlycon 会抢 UART0

双 OS 跑起来后 openvela 的输出整段消失，看起来像 openvela 没起来 ——
实际是它的字被 Linux 的 earlycon 逐个冲掉了。

`earlycon` 不是我加的，是**内核 DTB 自己带的**：`rk3576-linux.dtsi` 的
`/chosen/bootargs` 里有

    earlycon=uart8250,mmio32,0x2ad40000 console=ttyFIQ0 root=PARTUUID=...

而 U-Boot 的 `bootargs_add_dtb_dtbo()`（`arch/arm/mach-rockchip/board.c`）
会把内核 DTB 里的 `/chosen/bootargs` **追加**到 env 的 bootargs 上。所以
在 U-Boot 提示符下 `setenv bootargs 'panic=0'` 是**没用的** —— 内核实际
拿到的仍然带着那个 earlycon。

解法是在 AMP 的 DTS 里覆盖 `/chosen`：

```dts
&chosen {
        bootargs = "panic=0";
};
```

`panic=0` 在 AMP 下有额外的分量：Linux 没有 rootfs，挂载根必然 panic。
panic=0 让它停住而不是重启 —— **重启走 PSCI SYSTEM_RESET，会把整片 SoC
连 openvela 一起复位**。一侧的"正常错误处理"就是另一侧的灾难。

要看 Linux 日志时用 `linux/rk3576-kickpi-k7-amp-dbg.dts`，它把 UART0 让给
Linux，但**只能在不启 openvela 的隔离实验里用**。

## 已经跑到哪一步（2026-09-12）

**分核在两个方向上都验证过了。**

openvela 单跑（FIT 里不带 Linux）——一路到 NSH，稳住不挂，`ampctl status`
正常输出（"握手完成：否"，因为没有对端，正确）：

```
AMP: Brought up primary cpu[0, self] with state 0x12, entry 0x4a400000 ...OK
[CPU0] AMP: rptun 就绪 对端=linux vring0=47800000 vring1=47808000 ...
nsh> ampctl status
```

只启 Linux（UART0 让给它）——起在 A72 簇，只拉 A72 四个核：

```
AMP: linux fdt at 0x4f000000
[   18.818583] Booting Linux on physical CPU 0x0000000100 [0x411fd080]
Machine model: KICKPI-K7 AMP debug (Linux on A72, owns UART0)
[   19.043478] CPU1: Booted secondary processor 0x0000000101
[   19.044502] CPU2: Booted secondary processor 0x0000000102
[   19.045474] CPU3: Booted secondary processor 0x0000000103
[   19.050400] SMP: Total of 4 processors activated.
```

双 OS——Linux 侧的 rpmsg 已经上线，vring 地址和 openvela 逐字对上：

```
rockchip-rpmsg 47800000.rpmsg: rockchip rpmsg platform probe.
rockchip-rpmsg 47800000.rpmsg: assigned reserved memory node rpmsg-dma@47a00000
rockchip-rpmsg 47800000.rpmsg: rpdev vdev0: vring0 0x47800000, vring1 0x47808000
virtio_rpmsg_bus virtio0: rpmsg host is online
```

剩下的是 openvela 侧确认收到了对端的第一次门铃（`ampctl status` 的
"握手完成"变成"是"）。上一轮没看到，是因为 earlycon 抢了 UART0（见上），
已修。

## 卡住之后怎么恢复

openvela 卡死时板子既没有串口回显也没有 USB 设备，**只能按 RESET**。
复位后默认 bootcmd 会去启动 boot 分区里的 Android 镜像（已被 Image
覆盖），失败后停在 `=>`，正好可以继续调试 —— 这也是保留默认 bootcmd
的好处之一。

要回到"只跑 NuttX"的状态：把 `nsh` 配置编出来，用
`scripts/flash.sh --raw`（nuttx.bin 2.2MB + stub dtb，都在 32MB 以内），
并把 uboot 分区写回 `~/rk3576-amp/backup/uboot-current-working.img`
（那是带 raw-nuttx bootcmd 的那一份）。
