#!/usr/bin/env bash
# rk3576 / KICKPI-K7 端口地址一致性检查。
#
# 有几组值分散在不同文件里，没有任何机制保证同步，
# 改漏一处的表现都是"上板没输出、也没报错"——最难查的一类问题。
# 每次改地址后跑一遍。
set -u

# 用法：scripts/check-addr.sh [配置名]   默认 nsh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 队伍仓根
WS="$(cd "$ROOT/.." && pwd)"                              # openvela 工作区根
CHIP="$ROOT/chip/rk3576/chip.h"
BOARD="$ROOT/board/kickpi-k7"
# 比对哪个配置。有了 amp 配置之后，默认的 nsh 不再总是对的那一个 ——
# 拿 nsh 的 defconfig 去核 amp 的 .config，会在地址真的分叉时报"一致"。
CFG="${1:-nsh}"
DEFC="$BOARD/configs/$CFG/defconfig"
LD="$BOARD/scripts/dramboot.ld"
LOWPUTC="$ROOT/chip/rk3576/rk3576_lowputc.S"
MMAP="$ROOT/chip/rk3576/hardware/rk3576_memorymap.h"
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
    # defconfig 里没有这一项不算错：make savedefconfig 生成的是**最小**
    # defconfig，取默认值的项本来就不写（amp 配置就是这么存的）。
    # 真正的故障是"两边都写了但不一样"——当年 .config 残留
    # RAM_START=0x42000000 而 defconfig 是对的，正是这一种。
    if [ -z "$y" ]; then
      printf '  · %-32s defconfig 未列出（取默认值 %s）\n' "$k" "${x:-<无>}"
    elif [ "$x" != "$y" ]; then
      printf '  ✗ %-32s .config=%-14s defconfig=%s\n' "$k" "${x:-<缺>}" "$y"
      g0=1; fail=1
    fi
  done
  [ $g0 -eq 0 ] && echo "  ✓ 两边都写了的项全部一致"
  # config.h 是否比 .config 旧
  CH="$WS/nuttx/include/nuttx/config.h"
  if [ -f "$CH" ] && [ "$DOTC" -nt "$CH" ]; then
    echo "  ✗ include/nuttx/config.h 比 .config 旧 —— 执行 cd nuttx && make include/nuttx/config.h"
    fail=1
  fi
fi

# ---- 第 1 组：镜像加载地址 ----
#
# 2026-09-12 起 dramboot.ld 直接写 CONFIG_RAM_START（链接脚本链接前会过
# 一遍 cpp），加载地址只剩 .config 这一个出处。以前是三处各写一遍，
# 改漏一处的表现是"上板完全没输出且不报错"——最难查的一类问题。
# 这里改成检查那三处没有偷偷退回字面量。
c=$(grep -oP '^\s*\.\s*=\s*\K\S+' "$LD" | head -1)
echo "链接地址（唯一出处 = CONFIG_RAM_START；dramboot.ld 里不应再有字面量）"
printf '  %-44s %s\n' "dramboot.ld 起始地址表达式" "$c"
if [ "$c" = "CONFIG_RAM_START;" ]; then
  echo "  ✓ 链接脚本跟随 .config"
else
  echo "  ✗ dramboot.ld 里又出现了字面量地址 —— 它会和 .config 悄悄分叉"; fail=1
fi
if grep -q 'CONFIG_LOAD_BASE' "$CHIP" 2>/dev/null; then
  echo "  ✗ chip.h 里又冒出 CONFIG_LOAD_BASE —— 加载地址只该有一个出处"; fail=1
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
