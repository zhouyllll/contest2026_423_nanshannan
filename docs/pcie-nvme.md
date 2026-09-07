# M.2 SSD（PCIe + NVMe）勘察与移植方案

> 硬件参数全部出自**原厂 dtb** 与**官方 SDK**，没有从别的 RK 型号推断。
> 读法见 `notes/DEBUG-CASES.md` 与仓库 README 里"先读 SDK 再读 TRM"。

## 硬件

原厂 dtb（`~/boot-orig.img` 里提取）：

| 节点 | 状态 | 说明 |
|---|---|---|
| `/pcie@2a200000` | **okay** | `rockchip,rk3576-pcie` + `snps,dw-pcie`，M.2 就是它 |
| `/pcie@2a210000` | disabled | |
| `/sata@2a240000`、`/sata@2a250000` | disabled | 板上没走 SATA |
| `/phy@2b050000` | okay | naneng combphy，PCIe 用的是这颗 |
| `/phy@2b060000` | okay | 另一颗 |

`/pcie@2a200000` 的关键属性：

```
compatible      rockchip,rk3576-pcie / snps,dw-pcie
max-link-speed  2          (Gen2)
num-lanes       1          (x1)
clock-names     aclk_mst aclk_slv aclk_dbi pclk aux
interrupt-names msi sys pmc msg legacy err
phys            /phy@2b050000   phy-names = "pcie-phy"
reset-gpios     gpio@27320000 pin 25，高有效

reg
  0x2a200000  0x10000    DBI（DesignWare 寄存器块）
  0x22000000  0x400000   配置空间 4MB
  0x20000000  0x100000   apb

ranges
  config        CPU 0x20000000   1MB
  I/O           CPU 0x20100000   1MB
  mem32         CPU 0x20200000   14MB
  prefetch64    CPU 0x900000000  2GB
```

★ **地址窗口与我们的 MMU 映射的关系**：`CONFIG_DEVICEIO_BASEADDR`
= 0x20000000、192MB，覆盖到 0x2C000000。所以 DBI、配置空间、I/O、
mem32 **都在已映射范围内**，不用动 MMU。只有 2GB 的 prefetch64 窗口
（0x900000000）在外面 —— NVMe 的 BAR0 通常只有 16KB，落在 mem32
就够，不需要它。这一条要在分配 BAR 时守住，否则会分到映射外的地址，
表现为访问就挂死。

## 软件现状

| | |
|---|---|
| NuttX PCI 总线框架 | **有**（`drivers/pci/`，`pci_ops_s` 接口 + `pci_ecam.c` 可参照），但 `CONFIG_PCI` 当前未启用 |
| NuttX NVMe 驱动 | **完全没有** —— 搜遍 nuttx/apps/packages 无任何 nvme 代码 |
| 参考实现 | U-Boot：`drivers/nvme/nvme.c` 950 行、`drivers/pci/pcie_dw_rockchip.c` 827 行（含 rk3576） |

★ 仍然选 U-Boot 作参考而不是内核：裸机形态、轮询、不依赖中断与块层
抽象，和我们要做的事情形状一致。内核那份的大半篇幅是 blk-mq 对接。

## 移植方案

1. **`chip/rk3576/rk3576_pcie.c`** —— combphy 初始化、五路时钟、复位、
   复位 GPIO、链路训练、iATU 出入窗口；然后实现 `pci_ops_s`
   （配置读写走 iATU + DBI）并 `pci_register_controller()`。
2. **启用 `CONFIG_PCI`**，由 NuttX 的 `pci.c` 完成枚举与 BAR 分配。
3. **`drivers/nvme/`（新）** —— 按 PCI 驱动注册，绑 class `0x010802`：
   admin 队列、Identify Controller/Namespace、建 I/O 队列、PRP 读写，
   最后注册成 NuttX 块设备。

## 第一个判据

按和 HDMI 同样的纪律，**先做能给出决定性结论的一步**：把 PHY、时钟、
复位、复位 GPIO 备好之后**读 LTSSM 状态看链路有没有训练起来**。链路
不通的话，后面 iATU、枚举、NVMe 全是空转。

★ 前提：**板上要真的插着 M.2 SSD**。没有盘的话链路训练必然失败，
而那个失败和"驱动写错了"在现象上完全一样 —— 这种分不开的情况要在
动手之前排除掉，不能等到调不通了再回头怀疑。

---

# 附：WiFi/蓝牙（SKW6621S）的现状与结论

> 这一节记在这里是因为它和 M.2 一样，属于"查清楚了但没做完"的部分。

## 硬件（原厂 Android 实测，非推断）

```
SDIO 卡    mmc0:0001:1  vendor=0x1ffe device=0x6621  内部名 SV6160LITE
控制器映射 mmc0 -> 2a320000.mmc   mmc1 -> 2a310000.mmc(SD)   mmc2 -> 2a330000.mmc(eMMC)
控制器身份 Version ID 270a，256 深 FIFO，irq 105 —— 与我们读到的完全一致
枚举结果   new ultra high speed SDR104 SDIO card at address 0001
初始时钟   400000Hz，div = 0（分频在 CRU，控制器 CLKDIV 旁路）
蓝牙       走 SDIO 私有 ucom 通道：BTCMD=2 BTAUDIO=3 BTDATA=5
           /dev/BTBOOT 引导固件，不注册标准 hci
```

★ dtb 里的 `wifi_chip_type = "ap6256"` **是错的**，见 DEBUG-CASES 案例 25。

## 我们的状态

控制器地址、身份、FIFO 深度、时钟都与原厂一致，SD 卡实例在同一份代码下
工作正常。但 SDIO 实例上 **CMD5 与 CMD52 均返回：RESP_ERR 置位、无 RTO、
无 RCRC、RESP0 恒为 0** —— 卡确实在拉命令线，但控制器认为响应帧格式不对。

已排除：控制器选错（mmc0 确认就是 0x2a320000）、时钟频率、中断状态未清
（DWMMC_INT_ALL 漏清高 16 位已修）、分频位置（照原厂改成 CRU 分频，无变化）。

## 移植代价（源码齐全，在 SDK 里）

`external/rkwifibt/drivers/skw6621s/`，合计 99169 行：

| 部分 | 行数 | 评估 |
|---|---|---|
| SDIO 平台层（总线 + 固件引导 + ucom） | ~9k | 可移植，WiFi/蓝牙共同地基 |
| 蓝牙传输 `skw_btdriver.c` | 1160 | 薄，可对接 NuttX 的 bt_driver_s |
| WiFi | ~25k | **依赖 cfg80211，NuttX 无此层** |

★ 结论：蓝牙路径可行（约 1 万行）；**WiFi 的障碍不是行数，是 NuttX 缺少
  802.11 管理层** —— `skw_cfg80211.c` 那 6207 行是对着 Linux cfg80211 写的，
  没有对应物可落。

## 下一步（若继续）

~~采样/驱动相位寄存器~~ —— **已排除**。查过 RK3576 的 CRU：没有
SDIO/SDMMC 的 drv/sample 时钟，dtb 也只有 `biu`/`ciu` 两个时钟。
`dw_mmc-rockchip.c` 里那套 `rockchip_mmc_set_phase()` 是给有这两个时钟的
老 SoC 用的，本芯片不走那条路。（先查再实现，省下一次白工。）

剩下的方向，按值得程度排：

1. **逐条比对原厂 `skw_sdio_main.c` 的初始化寄存器序列**（尚未做）。
2. 复查 `USE_HOLD_REG`、`PRV_DAT_WAIT` 等命令位在 SDIO 实例上的取值 ——
   我们对两个实例一律相同，而 Rockchip 驱动会按速率/实例区别对待。
3. 用示波器看 CMD 线的实际波形。软件侧能查的判据基本用尽了，
   "卡在拉线但帧格式不对"再往下走需要物理层证据。
