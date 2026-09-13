# AMP：openvela（A53 簇）+ Linux（A72 簇）

RK3576 是 4×A53 + 4×A72。AMP 的目标是让两个簇各跑一个**完整的操作系统**：
openvela 拿 A53 做实时与产品控制，Linux 拿 A72 做 NPU / ISP / 重媒体，
两边通过共享内存 + 硬件门铃通信。

不是虚拟化，不是 hypervisor，也**不提供隔离**：两个 OS 平级、互信，
任何一边跑飞都可能拖垮另一边。这是协作式 AMP，取的是"低开销 + 各取所长"。

---

## 目前进展：✅ 双 OS 同时运行 + 屏上有界面，rpmsg 握手完成（2026-09-13）

| 层次 | 状态 | 证据 |
|---|---|---|
| mailbox 门铃驱动 | ✅ | `ampctl selftest` 正例+反例都过 |
| rptun / rpmsg 传输层 | ✅ | `ampctl status` 握手完成=是，`/dev/rpmsg/linux` 存在 |
| `up_addrenv_*` 地址转换 | ✅ | |
| GIC 共享 distributor | ✅ | Linux 侧 `rockchip,amp` + `amp-irqs`；openvela 侧按字节认领兜底 |
| Linux 内核 + 只含 A72 的 DTB | ✅ | 7.6MB（原 43MB），`SMP: Total of 4 processors activated` |
| 双 OS 引导（核拆分） | ✅ | `bootamp`，见 `../amp/uboot/` |
| **openvela(A53) + Linux(A72) 同时跑** | ✅ | `ps` 里四个 A53 的 IDLE + `rpmsg-linux-0` 线程 |
| 5 寸 MIPI 屏（AMP 路径） | ✅ | U-Boot 用自己的完整控制 dtb 点亮，openvela 接管：`/dev/fb0` 720x1280 |
| LVGL 仪表盘 | ✅ | `kickpi_ui` 开机自启，屏上四页（AMP/DEV/LIVE/ABOUT） |
| 触摸（FT8756）接进 LVGL | ✅ | ft5x06 补 `TSIOC_GETMAXPOINTS`，见 `../bsp/upstream/` |
| 端到端收发一帧 | ⬜ | 需要 Linux 侧用户态 |

### 这一轮踩到的三个「静默失败」

把 UI 接到 AMP 配置上的过程里，有三个 bug 的共同点是**没有任何一侧报错**，
现场看起来都指向别处。记在这里，因为它们的查法比结论更有价值：

| 现象 | 看起来像 | 实际是 |
|---|---|---|
| `booti` 在 "Booting using the fdt blob" 之后 data abort | dtb 坏了 | `bootm_disable_interrupts()` 对 `env_get("devtype")` 没判空；关掉 `USING_KERNEL_DTB` 后 `setup_boot_dev()` 跑在环境变量初始化之前，`Bootdev(atags)` 从 `mmc 0` 变成 `<NULL> <NULL>`（见 `../amp/uboot/0006`） |
| openvela 打印到 NSH 横幅后串口彻底安静 | 死机 | 活着但聋了：Linux 的 `gic_dist_config()` 无条件清掉所有 SPI 的使能位，把 openvela 的 UART0(108)/mailbox(174) 一起关了。启动期日志走轮询式 `up_putc()` 所以一直有输出，NSH 第一次走字符设备就卡住（见 `../amp/linux/0001`） |
| 界面出来了但触摸没反应 | 触摸驱动没起来 | `/dev/input0` 正常；LVGL 的 `lv_nuttx_touchscreen_create()` 第一件事是 `ioctl(TSIOC_GETMAXPOINTS)`，ft5x06 驱动没实现，返回 `-ENOTTY` 就放弃了，而 `LV_USE_LOG` 默认关着（见 `../bsp/upstream/ft5x06-*`） |

第二个之所以能分开"死了"和"聋了"，是因为把 LVGL 界面改成了开机自启 ——
它每 250ms 刷一次，成了一个**不依赖串口的旁路心跳**。串口哑而画面在动
= 活着但聋了；两个都停 = 真死了。没有这个旁路，光看串口两种情况一模一样。

板上输出与完整命令序列见 [../amp/README.md](../amp/README.md) 与
[../amp/FLASH.md](../amp/FLASH.md)；资源划分见
[../amp/OWNERSHIP.md](../amp/OWNERSHIP.md)。

---

## 参考了谁

同届队伍 **contest2026_062_PharosTech** 在 2026-09-10 已经把 4+4 双 OS 跑通
（见其 `tools/amp/BOARD_VALIDATION.md`）。本项目在动手前完整读过他们公开的
方案，以下判断直接采信了他们的板测结论，省掉了重新试错：

- openvela 站 rpmsg 的 **remote**，Linux 站 master；
- 握手的唯一信号是**对端的第一次门铃**，不是共享内存里的状态位；
- mailbox3 的 A2B 中断号是 **174**；
- Linux 的最终 DTB 里必须**删掉** A53 的 cpu 节点，`status = "disabled"`
  挡不住这版 vendor 内核枚举 CPU。

不同的地方：

- 他们用自研的 N-Boot 加 `bootamp` 命令做引导；本项目走 **U-Boot 自带的
  `CONFIG_ROCKCHIP_AMP`**（`drivers/cpu/rockchip_amp.c` + `amp.its`）。
  这不是"更好"，是手上已经有：`~/rk3576-amp/` 那棵树早就用
  `rk3576_defconfig + rk3576-amp.config` 编出过 `u-boot.img`（1.45MB，
  `nm` 里能看到 `amp_cpus_on` / `sip_smc_amp_cfg`），机制分析见
  [smp-amp.md](smp-amp.md)，编译环境见 [uboot-build.md](uboot-build.md)。
  少引入一个自研引导器，就少一个自己要维护的东西。
- GIC 补丁他们拆成两个 Kconfig，本项目合成一个，理由见下。
- 代码全部自己写，寄存器与中断号逐项回到 TRM / SDK 核对过（见下）。

---

## 线上契约

这条链路**不是**标准的 OpenAMP remoteproc 协议。Rockchip 的实现
（Linux `drivers/rpmsg/rockchip_rpmsg_mbox.c`）有三处关键差异：

1. **线上没有 resource table。** vring 地址写死在双方的 DTS / 头文件里。
2. **kick 就是一条 mailbox 消息** `{cmd = link_id, data = 0x524D5347}`。
3. **只宣告一个 feature**：`VIRTIO_RPMSG_F_NS`。

几何参数（`include/linux/rpmsg/rockchip_rpmsg.h`，两边必须逐字一致）：

| 项 | 值 |
|---|---|
| vring 大小 | 0x8000 |
| 描述符个数 | 64 |
| 对齐 | 0x1000 |
| buffer | 512 = 496 payload + 16 hdr |
| magic | `0x524D5347` "RMSG" |

本端的内存窗口（`CONFIG_RK3576_RPTUN_SHM_BASE`，默认 `0x47800000`）：

```
0x47800000  vring0   openvela → Linux
0x47808000  vring1   Linux → openvela
0x47a00000  缓冲池   2MB
0x47c00000  窗口结束
```

整段在 MMU 里映成 **Normal-NonCacheable**。理由：Linux 那边是 `ioremap()`
出来的，明确清掉了 `RPMSG_CACHED_VRING`。我们若映成 cacheable，就得对每一次
描述符读写做 cache 维护，漏一处的表现是偶发丢包或读到半新半旧的描述符 ——
不值得。vring 里只有 16 字节的描述符，不缓存的开销可以忽略。

---

## 硬件核对

每个常量都回到一手资料查过，没有从别处照抄：

| 项 | 值 | 出处 |
|---|---|---|
| MAILBOX 基址 | `0x2AE50000`，步长 0x1000，14 组 | TRM Part1 V1.2 §17.4.1 |
| 寄存器偏移 | A2B INTEN/STATUS/CMD/DATA = 0x00/04/08/0c；B2A = 0x10/14/18/1c | 同上 §17.4.2 |
| INTEN 位 | bit0 = 使能，bit8 = 写完 DATA 才触发（复位值 1），高 16 位写使能 | 同上 §17.4.3 |
| STATUS | bit0，W1C | 同上 |
| 中断号 | `irq_mailbox_ap0..13 = 157..170`，`bb0..13 = 171..184` | TRM 中断表 |
| 交叉验证 | dtsi `mailbox0 = GIC_SPI 125` → 125+32 = 157 ✓ | `rk3576.dtsi` |
| pclk 门控 | `CRU_GATE_CON17` bit13（1 = 关） | TRM p.123 |

**方向为什么是这样**：TRM §17.3 明说 AP / BB 只是叫法，谁站哪端由软件定。
实际约束来自 Linux —— `drivers/mailbox/rockchip-mailbox.c` 固定发 A2B、收
B2A。所以 Linux 是 AP，openvela 只能站 BB：发写 `B2A_*`，收用 `A2B_*` 和
`irq_mailbox_bbN`。记反的症状是"两边都在发，谁也收不到"，而寄存器读回来
全是对的。

---

## 握手：为什么要在中断里改 status

`rptun_dev_start()` 里有这么一段（`nuttx/drivers/rptun/rptun.c`）：

```c
role = IS_MASTER ^ (reserved[0] == VIRTIO_DEV_DRIVER);
if (role == VIRTIO_DEV_DEVICE && !(status & DRIVER_OK))
    return -EAGAIN;
```

device 一侧要等 driver 一侧把 `DRIVER_OK` 写进 resource table 才肯往下走。
可 Linux 压根不知道有这张表，这个位永远不会被置上，rptun 就一直 `-EAGAIN`。

真正等价的信号是 **Linux 的第一次 kick**：`rockchip_rpmsg_mbox.c` 里
`first_notify` 那段，Linux 建好两个 virtqueue、填好接收缓冲之后才发。收到它，
就说明对端确实已经 DRIVER_OK。所以在 mailbox 回调里补上本地表的 status，
再通知 rptun 重试。

这不是绕过检查 —— 是把检查的输入从"对端写的内存"换成"对端发的门铃"，
语义一致。（这个办法来自 PharosTech 的板测结论。）

---

## 为什么自检必须有反例

`ampctl selftest` 跑两遍同一条路径：写 CMD+DATA（应该进中断），和只写 CMD
不写 DATA（应该超时）。

只跑正例是不够的。一个永远返回"成功"的自检比没有自检更坏。本项目已经被
四个这样的假仪器坑过（`RXFIFOLR`、bank4 的 GPIO 探针、计数器没使能的
`RX_DATA_CNT`、参数未初始化的 SAI 回环，见 `notes/DEBUG-CASES.md`）。
每一个都给出了**自信的错误答案**，而不是"未知"——后者会让人继续查，
前者会让人拿着它下结论。所以仪器必须先证明自己会失败。

自检刻意用**空闲的 group5**，不用 rptun 的 group0/3：在 rptun 的 group 上
放假门铃，会被它当成"对端 DRIVER_OK 了"，于是拿着一片全是垃圾的 vring
往下走 —— 自检本身成功，却把系统推进了一个没有对端的非法状态。

---

## GIC：为什么只有一个开关

AMP 里两个 OS 共用一个 GIC-400。distributor（GICD）是**全局**的：SPI 的
使能、优先级、触发方式、目标 CPU 都放在按 IRQ 号打包的共享寄存器里，
一次 32 位写会连带覆盖相邻若干个 IRQ 的配置。所以它只能有一个所有者。

`CONFIG_ARM64_GICV2_SHARED_DIST=y` 让 NuttX 放弃这个所有权：

- 不复位 SPI，不写 `GICD_CTLR`；
- `up_prioritize_irq()` / `up_set_irq_type()` 对 SPI 改为**校验**——
  请求值与所有者配好的一致返回 OK，不一致返回 `-EPERM`；
- `up_affinity_irq()` 对 SPI 不写，只打一行日志。

CPU 接口（GICC）和 banked 的 SGI/PPI 每核一份，仍各自初始化。

PharosTech 把它拆成 `PREINITIALIZED` 和 `STATIC_SPI` 两个选项。本项目合成
一个，因为这两件事在 AMP 下必须同时成立；拆开就存在一种合法但致命的组合
（不复位、却仍去读改写），它的表现是**悄悄改掉对端某几个中断的配置**，
而本端一切正常 —— 这种故障没有任何本地症状可查。少一个开关就少一类这种
状态。

补丁在 `bsp/upstream/gicv2-shared-distributor.patch`，默认 `n`，普通配置
行为完全不变。**现在 openvela 单跑，这个选项要保持关闭** —— 此刻
distributor 归它自己。等 Linux 先起来并接管 GIC，再打开。

---

## 怎么编、怎么试

```bash
cd nuttx
./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/amp
make -j$(nproc)
cd .. && ./contest2026_423_nanshannan/scripts/flash.sh --raw
```

板上：

```
ampctl selftest   # 门铃收发路径，不需要对端
ampctl status     # 本端链路状态，不需要对端
ampctl ping       # 端到端，需要对端
ampctl listen     # 只收不发，需要对端
```

`ampctl` 的每条路径都有界，最长就是 `-t` 给的毫秒数，不会无限等。

---

## 还差什么

1. **Linux 侧**：用 K7 SDK 的 kernel-6.1 编一份 arm64 Image，DTB 里只留
   MPIDR `0x100..0x103` 四个 A72 节点（删掉 A53，不是标 disabled），
   打开 `rockchip_rpmsg_mbox` 并配 `rockchip,link-id = <3>`、
   `mboxes = <&mailbox0 0>, <&mailbox3 0>`，保留内存对上本文的窗口。
2. **引导**：U-Boot 的 `CONFIG_ROCKCHIP_AMP` + `amp.its` FIT，
   给 Linux 主核 `cpu = <0x100>`、openvela `cpu = <0>`。
3. **openvela 的加载地址**：现在链接在 `0x40480000`，会和 Linux 撞。
   双 OS 时要挪到保留区（PharosTech 用 `0x4a400000`），三处要一起改
   （`chip.h`、`defconfig`、`dramboot.ld`，`scripts/check-addr.sh` 会校验）。
4. **打开 `CONFIG_ARM64_GICV2_SHARED_DIST`**，并确认两边的 IRQ 不重叠。

---

## 一处踩过的坑（记下来免得再踩）

`repo sync` 会按**上游 manifest** 重建 `<linkfile>` 软链，把
`vendor/openvela/boards/contest2026_423_board` 指回模板里的
`board/contest_board`，于是 `configure.sh` 报"目录不存在"。同一次 sync 还
清掉了 `apps/graphics/lvgl/lvgl/`，重新解包后 LVGL 的 Kconfig 才第一次被
source 进来，`olddefconfig` 随即生成了几十个此前不存在的 `CONFIG_LV_*`，
其中 `CONFIG_LV_ATTRIBUTE_LARGE_CONST=""` 直接让 LVGL 编不过。

结论：**同步之后先跑 `scripts/check-addr.sh`，并确认软链指向没变**。
本 AMP 配置不带 LVGL（用不上），LVGL 的这个坑留在 `nsh` 配置里另行处理。
