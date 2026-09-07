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
