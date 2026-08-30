# KICKPI-K7 (RK3576) Board — openvela Port

[ English | [简体中文](README_zh-cn.md) ]

> **Status: skeleton in place, builds successfully, serial parameters confirmed from vendor sources, hardware bring-up pending**
>
> The SoC and board layers exist and `nuttx.bin` builds (entry `0x42000000`,
> carries an ARM64 Linux Image header, loadable by U-Boot `booti`).
> The debug console's UART instance, base address, clock and baud rate are
> confirmed from the vendor U-Boot defconfig in KICKPI's official Armbian tree
> — see "Confirmed" at the end.

## Board

- **Board**: KICKPI-K7
- **SoC**: Rockchip **RK3576** (4x Cortex-A72 @2.2GHz + 4x Cortex-A53 @2.0GHz)
- **Memory / storage**: 4/8/16 GB LPDDR, 16/32/64 GB eMMC
- **Network**: dual Gigabit Ethernet
- **Buttons**: RESET / POWER / RECOVERY / **MASKROM**

## Three ways RK3576 differs from its siblings

The three easiest mistakes when porting all come from copying another
Rockchip part:

| | RK3399 / RK3568 / RK3588 | **RK3576** |
|---|---|---|
| Interrupt controller | GIC-500 / GIC-600, **GICv3** | GIC-400, **GICv2** |
| DRAM physical base | RK3568 starts at `0x0` | **`0x40000000`** |
| Peripheral address range | high, `0xfxxxxxxx` | **low, `0x22000000`–`0x2b060000`** |

All three fail silently: nothing is printed and no error is reported.

- **GICv2**: set `CONFIG_ARM64_GIC_VERSION=2`. Setting 3 gives you working
  output but no input (interrupts never fire). References:
  `boards/arm64/zynq-mpsoc/zcu111` (also GIC-400) and the QEMU config
  `qemu-armv8a:nsh_gicv2`, which lets you exercise the GICv2 path without
  hardware.
- **Load address `0x42000000`**: from `kernel_addr_r` in mainline U-Boot
  `include/configs/rk3576_common.h`. Copying RK3568's `0x02000000` lands
  inside the peripheral region.
- **Peripheral window**: `CONFIG_DEVICEIO_BASEADDR = 0x20000000`, 192 MB.
  It must cover GIC, UART, CRU, GRF and GPIO, and must not overlap DRAM.

## What is ported

| Item | Status | Notes |
|---|---|---|
| Boot entry / MMU / exception vectors | Builds | `arch/arm64` common layer |
| GICv2 interrupt controller | Builds | `arm64_gicv2.c`, GICD `0x2a701000` / GICC `0x2a702000` |
| Generic Timer | Yes | ARM architectural, provided by the common layer |
| PSCI | Yes | `arm,psci-1.0`, `smc` method |
| Early print (lowputc) | Pending HW | DW 8250: poll LSR.THRE, write THR |
| Serial console | Pending HW | Generic `uart_16550.c`; DW 8250 is 16550-compatible |
| GPIO / I2C / SPI | No | To do |
| eMMC / SD | No | To do |
| Ethernet / USB / display | No | To do |
| NPU | No | Register-level documentation is not public |

Boot runs on A53 cluster core 0 (MPIDR `0x0`) only; SMP is not enabled.
Bringing up the A72 cluster (MPIDR `0x100`–`0x103`) is future work.

## Key hardware parameters

Sources: mainline Linux `arch/arm64/boot/dts/rockchip/rk3576.dtsi` and
mainline U-Boot `include/configs/rk3576_common.h` (both from Rockchip).

```
GICD              0x2a701000        GICC              0x2a702000
DRAM base         0x40000000        kernel_addr_r     0x42000000
SRAM/IRAM         0x3ff80000        CRU               0x27200000
UART0             0x2ad40000  SPI 76  -> IRQ 108   <- assumed debug port
UART2..9          0x2ad50000..0x2adc0000  SPI 78..85
UART10/11         0x2afc0000 / 0x2afd0000  SPI 86/87
UART1             0x27310000
GPIO0             0x27320000        GPIO1..4          0x2ae10000..0x2ae40000
```

Every UART is `snps,dw-apb-uart` with `reg-shift = <2>` and
`reg-io-width = <4>`, which maps to `CONFIG_16550_REGINCR=4` and
`CONFIG_16550_REGWIDTH=32`. **Omitting those two makes the driver step
registers by one byte, reading zeros or garbage — the port appears to
initialise successfully but never emits a character.**

> Under GICv2 the IRQ number is the dtsi `GIC_SPI` number plus 32.

## Build

```bash
cd <openvela workspace>
source build/envsetup.sh
cd nuttx
./tools/configure.sh -e ../vendor/rockchip/boards/rk3576/kickpi-k7/configs/nsh
make -j$(nproc)
```

The result is `nuttx.bin`, about 304 KB, carrying the ARM64 Linux Image
header (magic `ARMd` at offset `0x38`).

> Run `make distclean` before switching build targets. If `distclean` fails
> with `chip/Make.defs: No such file or directory` after a branch switch,
> those are dangling symlinks left by the previous configuration:
> ```bash
> rm -f arch/arm64/src/{chip,board} include/arch/{chip,board} .config Make.defs
> ```

## Flashing and running

The image is loaded by U-Boot as a Linux Image:

```
BootROM -> TPL(ddr.bin) -> SPL -> U-Boot -> booti
```

At the U-Boot prompt:

```
=> printenv kernel_addr_r          # expect 0x42000000; vendors may change it
=> tftp ${kernel_addr_r} nuttx.bin # or fatload / load mmc
=> md ${kernel_addr_r} 10          # confirm the image actually landed
=> booti ${kernel_addr_r} - ${fdt_addr_r}
```

**Always `md` before `booti`.** A wrong load address produces no output and
no error, which is the hardest failure to diagnose at this stage.

The board has a dedicated **MASKROM** button; together with `rkdeveloptool`
it recovers a bricked eMMC. **Walk through the recovery flow once before
writing any code.**

## Parameter status

### Confirmed (from the vendor U-Boot defconfig in KICKPI's official Armbian tree)

`patch/u-boot/legacy/u-boot-radxa-rk35xx/defconfig/kickpi-k7-rk3576_defconfig`:

```
CONFIG_DEBUG_UART=y
CONFIG_DEBUG_UART_BASE=0x2ad40000     -> UART0
CONFIG_DEBUG_UART_CLOCK=24000000      -> 24 MHz
CONFIG_DEBUG_UART_SHIFT=2             -> reg-shift=2, i.e. CONFIG_16550_REGINCR=4
CONFIG_BAUDRATE=1500000
```

| Item | Value |
|---|---|
| Debug console | **UART0, `0x2ad40000`, GIC_SPI 76 -> IRQ 108** |
| Input clock | **24 MHz** |
| Baud rate | **1500000** (not 115200) |
| Register stride / width | **4 / 32** |

### Still to be measured on hardware

| # | Item | Current value | How to confirm |
|---|---|---|---|
| 1 | `kernel_addr_r` | `0x42000000` (mainline U-Boot `rk3576_common.h`) | `printenv kernel_addr_r` on the board; vendors may change it |
| 2 | DRAM size | conservatively 512 MB | board specification; too small only wastes memory |
| 3 | LED / button GPIOs | not filled in | K7 schematic |

## References

| Resource | Link |
|------|------|
| Getting started / wiki | https://doc.kickpi.cn/Products/Beginner-Guide/KICKPI-K7/ |
| Hardware documentation | https://doc.kickpi.cn/Products/Introduction/KICKPI-K7/ |
| Peripherals (CAN etc.) | https://doc.kickpi.cn/Products/Peripherals-and-Interfaces/CAN/ |
| K7 RK3576 file share (Baidu) | https://pan.baidu.com/s/1cMKQt06pWdxZcsOp1XIvQA?pwd=kpcd (code: `kpcd`) |
