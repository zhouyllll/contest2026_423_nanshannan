#!/usr/bin/env bash
# 从设备树里取出一个节点的时钟信息，并把「门控」与「必须自己设的频率」分开。
#
# 为什么需要它
# ------------
# dtsi 里 `clocks = <...>` 和 `assigned-clock-rates = <...>` 看起来都是
# "这个外设的时钟"，但对裸机/RTOS 移植来说性质完全不同：
#
#   clocks              → 这些时钟你至少要开门控
#   assigned-clock-*    → **Linux 的时钟框架启动时会去设的目标值**。
#                         裸机上没有那套框架，不自己设就是引导器留下的任意值。
#
# 只照着 clocks 开门控、以为频率就是 assigned-clock-rates 写的那个数，
# 是本 skill 记录的头号错误（实测曾导致整条音频链自洽地跑在 1/4 频率上，
# 全程无任何报错）。
#
# 用法：
#   ./clock-intent.sh <dtsi 或 dts 文件> <节点名前缀>
#   ./clock-intent.sh rk3576.dtsi sai1
#   ./clock-intent.sh rk3576.dtsi i2c2
#
# 文件可以是内核源码树里的 .dtsi，也可以是板上 dtc 反编译出来的 .dts。
set -euo pipefail

DTS="${1:-}"
NODE="${2:-}"

if [ -z "$DTS" ] || [ -z "$NODE" ]; then
  echo "用法: $0 <dtsi/dts 文件> <节点名前缀>" >&2
  echo "例:   $0 rk3576.dtsi sai1" >&2
  exit 1
fi

[ -f "$DTS" ] || { echo "找不到文件: $DTS" >&2; exit 1; }

# 取出该节点的整段（从 "<node>...{" 到配对的 "};"，按缩进近似匹配）
BLOCK=$(awk -v n="$NODE" '
  $0 ~ "(^|[[:space:]])" n "[:@ ]" && /\{/ { inb=1; depth=0 }
  inb {
    print
    depth += gsub(/\{/, "{")
    depth -= gsub(/\}/, "}")
    if (depth <= 0) { inb=0 }
  }
' "$DTS")

if [ -z "$BLOCK" ]; then
  echo "在 $DTS 里没找到节点 '$NODE'" >&2
  exit 1
fi

echo "===== 节点 $NODE ====="
echo "$BLOCK" | head -1
echo

echo "--- 需要开门控的时钟（clocks / clock-names）---"
echo "$BLOCK" | grep -E "clocks[[:space:]]*=|clock-names[[:space:]]*=" || echo "  （无）"
echo

echo "--- ★ 需要你自己设频率的（assigned-clock-*）---"
ASSIGNED=$(echo "$BLOCK" | grep -E "assigned-clock" || true)
if [ -n "$ASSIGNED" ]; then
  echo "$ASSIGNED"
  echo
  echo "  ⚠ 以上是 Linux 时钟框架**会去设**的目标值，不是上电现状。"
  echo "    裸机/RTOS 上必须自己写 mux 与 divider，并读回确认。"
  echo "    只开门控就假定频率正确 = 用引导器留下的任意值。"
else
  echo "  （无 assigned-clock-*）"
  echo
  echo "  注意：没有 assigned-clock-* 不代表频率一定对 —— 也可能是父节点"
  echo "        或 clk 驱动里的 default rate 在起作用。仍需实测验证。"
fi
echo

# ★ 频率约束常常挂在**消费者**节点上，不在提供时钟的节点上。
#   例：sai1 自己只有 clocks=<&cru MCLK_SAI1_8CH>，一个频率都没写；
#   而 assigned-clock-rates=<12288000> 在引用 mclkout_sai1 的 es8388
#   节点里。只读提供者节点必然漏掉这个约束。
echo "--- ★ 谁引用了本节点导出的时钟（反向搜消费者）---"
CLKOUT=$(echo "$BLOCK" | grep -oE "[a-z0-9_]*(mclkout|clkout|_out)[a-z0-9_]*" | sort -u)
LABEL=$(echo "$NODE" | sed 's/[:@].*//')
PATTERNS="$LABEL"
[ -n "$CLKOUT" ] && PATTERNS="$PATTERNS $CLKOUT"
FOUND=0
for pat in $PATTERNS; do
  HITS=$(grep -n "&${pat}\b" "$DTS" | grep -iE "clocks|assigned" || true)
  if [ -n "$HITS" ]; then echo "$HITS"; FOUND=1; fi
done
if [ "$FOUND" = 0 ]; then
  echo "  （本文件内没找到引用；板级 dts 是另一个文件，要在那边再搜一次）"
  echo "  搜法： grep -rn 'assigned-clock-rates' <板级 dts 目录>"
fi
echo

echo "--- 复位（resets / reset-names）---"
echo "$BLOCK" | grep -E "resets[[:space:]]*=|reset-names[[:space:]]*=" || echo "  （无）"
echo

echo "--- 提醒：板级外给时钟（晶振、别的芯片的 CLKOUT）不在设备树里，"
echo "          必须查原理图。组合无线模组尤其常见缺 32.768kHz。"
