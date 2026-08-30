#!/usr/bin/env bash
# 按 CPU 核心特征，在 Linux 主线里定位目标 SoC 的参考设备树。
#
# 适配一颗 openvela/NuttX 没有支持过的 SoC 时，最省力的硬件参数来源是
# Linux 主线的 dtsi —— 但同一厂商往往有十几份 dtsi，选错就全盘皆错。
# 本脚本用「CPU 核心型号与数量」这个指纹去筛，通常能唯一命中。
#
# 用法：
#   ./find-reference-dtsi.sh <厂商目录> [核心型号]
#   ./find-reference-dtsi.sh amlogic cortex-a510
#   ./find-reference-dtsi.sh rockchip            # 不给核心型号则列出全部
#
# 厂商目录取值见 arch/arm64/boot/dts/ 下的子目录名：
#   amlogic / rockchip / allwinner / freescale / qcom / mediatek / ti ...
set -euo pipefail

VENDOR="${1:-}"
WANT="${2:-}"
BASE="https://raw.githubusercontent.com/torvalds/linux/master/arch/arm64/boot/dts"
API="https://api.github.com/repos/torvalds/linux/contents/arch/arm64/boot/dts"

if [ -z "$VENDOR" ]; then
  echo "用法: $0 <厂商目录> [核心型号]" >&2
  echo "例:   $0 amlogic cortex-a510" >&2
  exit 1
fi

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

echo "拉取 $VENDOR 的 dtsi 列表..."
files=$(curl -sf "$API/$VENDOR" | grep -oP '"name": "\K[^"]+\.dtsi' || true)
[ -n "$files" ] || { echo "取不到列表，检查厂商目录名是否正确" >&2; exit 1; }

printf '\n%-34s %s\n' "dtsi" "CPU 核心"
printf '%s\n' "--------------------------------------------------------------"

hits=""
for f in $files; do
  curl -sf -o "$TMP/$f" "$BASE/$VENDOR/$f" || continue
  cores=$(grep -oP 'compatible = "arm,\K[a-z0-9-]+' "$TMP/$f" 2>/dev/null \
          | sort | uniq -c | awk '{printf "%s×%s ", $1, $2}')
  [ -n "$cores" ] || continue
  mark=""
  if [ -n "$WANT" ] && echo "$cores" | grep -qi "${WANT#arm,}"; then
    mark=" ★"
    hits="$hits $f"
  fi
  printf '%-34s %s%s\n' "$f" "$cores" "$mark"
done

if [ -n "$WANT" ]; then
  echo
  n=$(echo $hits | wc -w)
  if [ "$n" -eq 0 ]; then
    echo "✗ 没有 dtsi 匹配 $WANT。核对核心型号，或该 SoC 尚未进主线。"
    exit 1
  elif [ "$n" -eq 1 ]; then
    echo "✓ 唯一命中：$hits"
  else
    echo "⚠ 命中 $n 份：$hits"
    echo "  需用第二个指纹再筛（核数、GICR 长度、外设组合）——"
    echo "  见 references/cross-checks.md"
  fi
  echo
  echo "下一步：务必做交叉验证再采用，不要直接抄地址。"
  echo "  最有效的一条：GICR 长度 ÷ 0x20000 应等于核数。"
fi
