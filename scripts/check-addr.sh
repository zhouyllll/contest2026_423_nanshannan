#!/usr/bin/env bash
# rk3576 / KICKPI-K7 端口地址一致性检查。
#
# 有几组值分散在不同文件里，没有任何机制保证同步，
# 改漏一处的表现都是"上板没输出、也没报错"——最难查的一类问题。
# 每次改地址后跑一遍。
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 队伍仓根
WS="$(cd "$ROOT/.." && pwd)"                              # openvela 工作区根
CHIP="$WS/nuttx/arch/arm64/include/rk3576/chip.h"
BOARD="$ROOT/board/kickpi-k7"
DEFC="$BOARD/configs/nsh/defconfig"
LD="$BOARD/scripts/dramboot.ld"
LOWPUTC="$WS/nuttx/arch/arm64/src/rk3576/rk3576_lowputc.S"
MMAP="$WS/nuttx/arch/arm64/src/rk3576/hardware/rk3576_memorymap.h"
DOTC="$WS/nuttx/.config"                                  # ★ 实际参与编译的配置

for f in "$CHIP" "$DEFC" "$LD" "$LOWPUTC" "$MMAP"; do
  [ -f "$f" ] || { echo "找不到 $f —— 端口是否已应用？见 scripts/sync-bsp.sh"; exit 1; }
done

norm() { printf '0x%x' "$((${1:-0}))" 2>/dev/null || echo "?"; }
fail=0

# ---- 第 0 组：.config 与 defconfig 是否同步（★ 最重要）----
#
# 编译真正读的是 nuttx/.config，不是 configs/nsh/defconfig。
# defconfig 只是模板，改了它不会自动生效。曾经因为这一点，
# .config 里残留 RAM_START=0x42000000 / RAM_SIZE=512MB，
# 堆被算成 539MB 越过映射边界，kumm_initialize() 静默挂死，
# 而本脚本当时只看 defconfig，一路报"✓ 一致"。见 DEBUG-CASES 案例 6。
#
# 同步方法（注意 make olddefconfig 在本工程跑不通，见案例 6）：
#   改 .config 后执行  cd nuttx && make include/nuttx/config.h
echo ".config 与 defconfig 同步性（编译实际读 .config）"
if [ ! -f "$DOTC" ]; then
  echo "  ⚠ 找不到 $DOTC —— 尚未配置过，跳过本组"
else
  KEYS="CONFIG_RAM_START CONFIG_RAM_SIZE CONFIG_ARM64_GIC_VERSION
        CONFIG_16550_UART0_BASE CONFIG_16550_UART0_CLOCK CONFIG_16550_UART0_IRQ
        CONFIG_16550_UART0_BAUD CONFIG_16550_REGINCR CONFIG_16550_REGWIDTH
        CONFIG_ARCH_BOARD_CUSTOM_DIR"
  g0=0
  for k in $KEYS; do
    x=$(grep -m1 "^$k=" "$DOTC"  | cut -d= -f2-)
    y=$(grep -m1 "^$k=" "$DEFC"  | cut -d= -f2-)
    if [ "$x" != "$y" ]; then
      printf '  ✗ %-32s .config=%-14s defconfig=%s\n' "$k" "${x:-<缺>}" "${y:-<缺>}"
      g0=1; fail=1
    fi
  done
  [ $g0 -eq 0 ] && echo "  ✓ 关键项全部同步"
  # config.h 是否比 .config 旧
  CH="$WS/nuttx/include/nuttx/config.h"
  if [ -f "$CH" ] && [ "$DOTC" -nt "$CH" ]; then
    echo "  ✗ include/nuttx/config.h 比 .config 旧 —— 执行 cd nuttx && make include/nuttx/config.h"
    fail=1
  fi
fi

# ---- 第 1 组：镜像加载地址（3 处）----
a=$(grep -oP '^#define\s+CONFIG_LOAD_BASE\s+\K0x[0-9a-fA-F]+' "$CHIP")
b=$(grep -oP '^CONFIG_RAM_START=\K0x[0-9a-fA-F]+' "$DEFC")
c=$(grep -oP '^\s*\.\s*=\s*\K0x[0-9a-fA-F]+' "$LD" | head -1)
echo "链接地址（= DRAM 基址 0x40000000 + text_offset 0x480000；U-Boot 从 kernel_addr_r=0x40400000 搬运至此）"
printf '  %-44s %s\n' "chip.h CONFIG_LOAD_BASE"      "$(norm $a)"
printf '  %-44s %s\n' "defconfig CONFIG_RAM_START"   "$(norm $b)"
printf '  %-44s %s\n' "dramboot.ld 起始地址"          "$(norm $c)"
if [ "$(norm $a)" = "$(norm $b)" ] && [ "$(norm $b)" = "$(norm $c)" ]; then
  echo "  ✓ 三处一致"
else
  echo "  ✗ 不一致！上板会完全没有输出且不报错"; fail=1
fi

# ---- 第 2 组：调试串口基址（2 处：汇编字面量 vs defconfig）----
d=$(grep -oP '^#define\s+UART_DBG_BASE_ADDRESS\s+\K0x[0-9a-fA-F]+' "$LOWPUTC")
e=$(grep -oP '^CONFIG_16550_UART0_BASE=\K0x[0-9a-fA-F]+' "$DEFC")
echo "调试串口基址"
printf '  %-44s %s\n' "rk3576_lowputc.S UART_DBG_BASE_ADDRESS" "$(norm $d)"
printf '  %-44s %s\n' "defconfig CONFIG_16550_UART0_BASE"       "$(norm $e)"
if [ "$(norm $d)" = "$(norm $e)" ]; then
  echo "  ✓ 两处一致"
else
  echo "  ✗ 不一致！early print 与串口驱动会打到不同地址"; fail=1
fi

# ---- 第 3 组：GIC 版本必须是 2 ----
gv=$(grep -oP '^CONFIG_ARM64_GIC_VERSION=\K[0-9]+' "$DEFC" || echo "")
echo "中断控制器版本（RK3576 是 GIC-400 = GICv2）"
printf '  %-44s %s\n' "defconfig CONFIG_ARM64_GIC_VERSION" "${gv:-未设置}"
if [ "$gv" = "2" ]; then
  echo "  ✓ GICv2"
else
  echo "  ✗ 必须是 2。RK3399/RK3568/RK3588 是 GICv3，RK3576 不是。"
  echo "    设错的表现：能打印但收不到输入（中断永不触发）"; fail=1
fi

# ---- 第 4 组：DW 8250 的寄存器访问方式 ----
ri=$(grep -oP '^CONFIG_16550_REGINCR=\K[0-9]+' "$DEFC" || echo "")
rw=$(grep -oP '^CONFIG_16550_REGWIDTH=\K[0-9]+' "$DEFC" || echo "")
echo "16550 寄存器访问（dtsi: reg-shift=2, reg-io-width=4）"
printf '  %-44s %s\n' "CONFIG_16550_REGINCR"  "${ri:-未设置}"
printf '  %-44s %s\n' "CONFIG_16550_REGWIDTH" "${rw:-未设置}"
# ★ 实际步长 = REGINCR * sizeof(uart_datawidth_t)，见 u16550_serialout()：
#       offset *= (priv->regincr * sizeof(uart_datawidth_t));
#   REGWIDTH=32 已经贡献了 sizeof=4，所以 REGINCR 必须是 1，合起来步长 4，
#   正好对应 dtsi 的 reg-shift=2。
#   曾经误填 REGINCR=4，步长变成 16，驱动把 IER 写到了 MCR 的位置，
#   串口初始化"成功"却一个字都发不出。见 DEBUG-CASES 案例 8。
if [ "$ri" = "1" ] && [ "$rw" = "32" ]; then
  echo "  ✓ 实际步长 = 1 * 4 = 4 字节，与 reg-shift=2 对应"
else
  echo "  ✗ 应为 REGINCR=1、REGWIDTH=32（步长 = REGINCR * REGWIDTH/8）。"
  echo "    REGINCR 会与 REGWIDTH 相乘，填 4 会得到 16 字节步长，"
  echo "    所有寄存器访问整体偏移，表现为初始化正常但收发全哑"; fail=1
fi

# ---- 第 5 组：外设地址是否落在 MMU 映射范围内 ----
base=$(grep -oP '^#define\s+CONFIG_DEVICEIO_BASEADDR\s+\K0x[0-9a-fA-F]+' "$CHIP")
mb=$(grep -oP '^#define\s+CONFIG_DEVICEIO_SIZE\s+MB\(\K[0-9]+' "$CHIP")
if [ -n "$base" ] && [ -n "$mb" ]; then
  end=$(( base + mb * 1024 * 1024 ))
  echo "外设映射范围 $(norm $base) .. $(printf '0x%x' $end)（${mb}MB）"
  bad=0
  check_in() {  # $1=名字 $2=地址
    [ -n "$2" ] || return 0
    if [ "$((${2}))" -lt "$((base))" ] || [ "$((${2}))" -ge "$end" ]; then
      echo "  ✗ 越界: $1 = $(printf '0x%x' $((${2})))"; bad=1; fail=1
    fi
  }
  check_in "UART_DBG" "$d"
  check_in "GICD" "$(grep -oP 'CONFIG_GICD_BASE\s+\K0x[0-9a-fA-F]+' "$CHIP")"
  check_in "GICC" "$(grep -oP 'CONFIG_GICR_BASE\s+\K0x[0-9a-fA-F]+' "$CHIP")"
  check_in "CRU"  "$(grep -oP 'RK3576_CRU_ADDR\s+\K0x[0-9a-fA-F]+' "$MMAP")"
  check_in "GPIO0" "$(grep -oP 'RK3576_GPIO0_ADDR\s+\K0x[0-9a-fA-F]+' "$MMAP")"
  check_in "GPIO4" "$(grep -oP 'RK3576_GPIO4_ADDR\s+\K0x[0-9a-fA-F]+' "$MMAP")"
  [ $bad -eq 0 ] && echo "  ✓ UART / GIC / CRU / GPIO 均落在映射范围内"
fi

# ---- 第 6 组：外设窗口与 DRAM 不得重叠 ----
ram=$(grep -oP '^#define\s+CONFIG_RAMBANK1_ADDR\s+\K0x[0-9a-fA-F]+' "$CHIP")
rmb=$(grep -oP '^#define\s+CONFIG_RAMBANK1_SIZE\s+MB\(\K[0-9]+' "$CHIP")
if [ -n "$ram" ] && [ -n "$rmb" ] && [ -n "$base" ] && [ -n "$mb" ]; then
  rend=$(( ram + rmb * 1024 * 1024 ))
  dend=$(( base + mb * 1024 * 1024 ))
  echo "DRAM $(norm $ram) .. $(printf '0x%x' $rend)（${rmb}MB）"
  if [ "$((ram))" -lt "$dend" ] && [ "$((base))" -lt "$rend" ]; then
    echo "  ✗ 与外设窗口重叠！RK3576 的外设在低位（0x22000000~0x2b060000），"
    echo "    DRAM 从 0x40000000 起，别照抄 RK3568 的 0x02000000"; fail=1
  else
    echo "  ✓ 与外设窗口不重叠"
  fi
fi

exit $fail
