# 自己编的 U-Boot（RK3576 / KICKPI-K7）

**已上板验证**（2026-09-12）：拿到 U-Boot 命令行，`bootdelay=3`，
`Ctrl+C` 可打断自动启动，`mmc part` / `printenv` 等命令可用。

```
U-Boot 2017.09 (Sep 12 2026 - 13:44:31 +0800)
aarch64-none-linux-gnu-gcc 10.3.1 20210621
```

## 为什么要自己编

出厂 U-Boot 是**静默**的：`CONFIG_BOOTDELAY=0`，没有任何打断机会，
不能 `setenv`、不能交互调试。板子一挂只能物理 recovery —— 这个代价在
本项目的调试里反复付出过。

## 复现步骤

SDK 工作树没有检出（只有 5.1GB 的 .git），所以先导出需要的三块：

```sh
SDK=<...>/rk3576-linux-20260320/rk3576-linux
W=~/rk3576-uboot; mkdir -p $W && cd "$SDK"
git archive --format=tar HEAD u-boot                            | tar x -C $W
git archive --format=tar HEAD rkbin                             | tar x -C $W
git archive --format=tar HEAD prebuilts/gcc/linux-x86/aarch64   | tar x -C $W
```

打补丁：

```sh
cd $W/u-boot
patch -p0 < <本目录>/0001-defconfig-bootdelay.patch      # BOOTDELAY 0->3
patch -p0 < <本目录>/0002-make_fit_atf-python3.patch     # py2 -> py3
```

准备两个垫片（见下）后编译：

```sh
export PATH=$W/bin:$W/u-boot/scripts/dtc:$PATH
TC=$W/prebuilts/gcc/linux-x86/aarch64/gcc-arm-10.3-*/bin/aarch64-none-linux-gnu-
./make.sh rk3576 CROSS_COMPILE=$TC
```

产物 `uboot.img` **4194304 字节**，FIT 节点 `uboot/atf-1/atf-2/atf-3/optee`
—— 与出厂镜像大小和结构完全一致。

## 编译路上的四个坑

| 坑 | 表现 | 解法 |
|---|---|---|
| SDK 工作树未检出 | 没有源码可编 | `git archive` 导出（不要 checkout，要十几 GB） |
| `make.sh` 写死 linaro 6.3 路径 | `No find aarch64-linux-gnu-gcc` | 传 `CROSS_COMPILE=`（它支持） |
| 缺 `dtc` | `ERROR: No 'dtc'` | 用 u-boot 自带的 `scripts/dtc/dtc`，不必装系统包 |
| `make_fit_atf.py` 是 **python2** | `ERROR: No python2` | 转 python3（补丁 0002）+ python2 垫片指向 python3 |

**转 python3 时最容易错的一处**：py2 的 `print >> f, x,` 末尾逗号表示
**不换行**（用于把 `loadables = "uboot", "atf@1", ...` 拼在同一行）。
直接删逗号会多出换行 —— DTS 虽然对空白不敏感，但语义变了。补丁里
对这 3 处补了 `end=''`。

## 两个垫片

- `python2-shim.sh` —— `make.sh` 硬性检查 `which python2`。垫片转发给
  python3，并带上装了 pyelftools 的 `PYTHONPATH`。
  （pyelftools 装法：`pip install --target=$W/pylibs pyelftools`，
  本机 PEP 668 禁止直接装系统 site-packages。）
- `fdtget-shim.sh` —— `fit-core.sh` 只用 `fdtget` 读一个**装饰性**的版本号。
  垫片只覆盖这一个用法，**其余用法一律大声失败**：fdtget/fdtput 的实质
  用途全在签名分支里（本项目不签名，启动日志是 `OK_NOT_SIGNED`），
  万一哪天走到那条路，报错比悄悄返回空值安全得多。

## 烧写（只动 uboot 分区）

**动 bootloader 之前必须先验证恢复路径。** 本次的做法是**先读回再写**：

```sh
rkdeveloptool ppt                                   # uboot 在 LBA 0x4000，8192 扇区
rkdeveloptool rl 16384 8192 /tmp/readback.img       # 读回当前内容
cmp /tmp/readback.img ~/rk3576-amp/backup/uboot-orig.img   # 与备份逐字节比对
```

比对一致 ⇒ **寻址正确、备份就是板上当前内容**，恢复命令因此是确定的。
这比"假设备份有效"可靠 —— 而且不需要先把板子弄砖来验证。

```sh
rkdeveloptool wl 16384 uboot.img                    # 写
rkdeveloptool rl 16384 8192 /tmp/verify.img         # 再读回
cmp /tmp/verify.img uboot.img                       # 校验
rkdeveloptool rd
```

**恢复**：`rkdeveloptool wl 16384 ~/rk3576-amp/backup/uboot-orig.img`
（若连 loader 模式都进不去，走 maskrom + `rk3576_spl_loader_v1.09.108.bin`）

**不碰**：SPL/idblock 分区（兜底）、rkbin、GPT。

## 波特率：一个没能改成的东西

改了 `CONFIG_BAUDRATE=115200`，环境变量里也确实是 115200，但**实际串口
仍跑 1500000** —— 控制台速率来自设备树里 `&cru SCLK_UART0` 算出的分频，
`CONFIG_BAUDRATE` 只是没有 DT 指定时的默认值。

后来判断**不该改**：TPL/SPL/ATF/OP-TEE 全是 rkbin 的闭源二进制，波特率
写死 1.5M 改不了。U-Boot 跟着 1.5M，整个引导阶段的日志才是一致的；
只有 NuttX 用 115200（那里要传文件、要长日志，1.5M 下实测丢 7~16% 字节）。

**所以现在是两段速率**：引导阶段 1500000，NuttX 起来后 115200。
与 U-Boot 交互要用 1.5M，`scripts/flash.sh` 给 NSH 发 `loader` 用 115200。

---

# 路径 B：直接启动裸 nuttx.bin（已上板验证 2026-09-12）

**结果**：上电后**全自动**从裸镜像进 NSH，不再经过 Android boot.img。

## boot 分区布局（GPT: boot @LBA 0xC800，64MB）

```
+0x0000 (LBA 0xC800)  nuttx.bin   标准 arm64 Linux Image（MZ…ARMd，text_offset 0x480000）
+0x2000 (LBA 0xE800)  booti-stub.dtb  344 字节，仅为满足 booti
```

## bootcmd（补丁 0003）

```
mmc dev 0;
mmc read ${kernel_addr_r} 0xC800 0x2000;
mmc read 0x4a000000 0xE800 0x400;
booti ${kernel_addr_r} - 0x4a000000;
RKIMG_BOOTCOMMAND            ← 失败时回落到原厂启动链，兜底
```

## 两个必须知道的坑

**一、arm64 的 `booti` 不能没有设备树。**

第三个参数给 `-` 时 U-Boot 仍会去解析 FDT，实测直接在**它自己身上**
Data Abort：

```
Fdt Ramdisk skip relocation
"Synchronous Abort" handler, esr 0x96000006
* PC = 0000000040226b28      ← U-Boot 地址段，不是 NuttX
```

NuttX 的地址全是硬编的、根本不读设备树，但这一关得过 —— 所以在分区里
带一份 FDT 纯粹是为了喂给 U-Boot。

**它只需要结构合法，不需要真实的板级描述。** 起初塞的是从原厂固件里抠出来
的 `board.dtb`（264KB）—— 那让启动链依赖一个厂商二进制，纯属浪费。现在用
自己的源码 `board/kickpi-k7/scripts/booti-stub.dts` 编出的 **344 字节**
stub（只有 `/chosen` 和一个 `/memory` 节点），已上板验证可自动启动。

两条更省的路都试过，不通：

| 做法 | 结果 |
|---|---|
| `booti addr - -`（完全不给 FDT） | U-Boot 自己 Data Abort |
| `booti addr - ${fdtcontroladdr}` | 该变量在本构建里未定义，展开成空，同样崩 |

还有 `go 0x40480000`（直接跳转、不要 FDT）没试 —— `go` 不做
`cleanup_before_linux()`（关 cache/MMU、关中断），等于跳过 arm64 的启动
协议。为省 344 字节去换一个不确定的启动状态不划算。

**二、FDT 别加载到 `fdt_addr_r`。**

`fdt_addr_r=0x48300000` 落在 Rockchip sysmem 的保留区里（memory.rgn[0]
= 0x40200000-0x48400000），`booti` 会报

```
Sysmem Error: Found there is region overflow!
```

虽然仍能启动，但换到 `0x4a000000`（空闲区 rgn[1]）就没有这条告警。

## 地址是怎么定的（不要照抄，要会算）

```
nuttx.bin 头 +0x08  text_offset = 0x00480000
DRAM 基址           0x40000000
                  → 0x40480000 = CONFIG_RAM_START（nuttx/.config 实测一致）
kernel_addr_r       0x40400000   booti 从这里读 Image 头，自己搬到 0x40480000
```

`mmc read` 读 0x2000 扇区（4MB）是按**分区布局的上限**取的，不是按当前
镜像大小 —— nuttx.bin 长大时不必改 bootcmd，booti 按 Image 头里的实际
大小处理。

## 烧写

```sh
# 内核（裸镜像，不打包）
rkdeveloptool wl 51200 nuttx.bin
# 设备树（分区内偏移 4MB）
rkdeveloptool wl 59392 board.dtb
```

`scripts/flash.sh --raw` 走这条路；不带参数仍走 Android boot.img 壳（路径 A
时期的方式），两条路都保留，便于对照与回退。
