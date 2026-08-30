#!/usr/bin/env bash
# 在 QEMU 中运行 qemu-armv8a 构建产物。
#
# 用法：scripts/run-qemu.sh [gic版本] [nuttx 路径]
#   scripts/run-qemu.sh 2      跑 GICv2（对应 qemu-armv8a:nsh_gicv2）
#   scripts/run-qemu.sh 3      跑 GICv3（对应 qemu-armv8a:nsh，默认）
#
# ★ RK3576 用的是 GIC-400，即 GICv2。板子到位前，用 gic-version=2 的
#   QEMU 把 GICv2 代码路径跑熟，是唯一不依赖硬件的技术推进项。
#
# 退出：Ctrl-A 然后按 x
set -e
GIC="${1:-3}"
NUTTX="${2:-$(cd "$(dirname "$0")/../.." && pwd)/nuttx/nuttx}"
[ -f "$NUTTX" ] || { echo "找不到 $NUTTX，先 configure + make"; exit 1; }
echo "== gic-version=$GIC  $NUTTX =="
exec qemu-system-aarch64 -cpu cortex-a53 -nographic \
  -machine "virt,virtualization=on,gic-version=$GIC" \
  -net none -chardev stdio,id=con,mux=on -serial chardev:con \
  -mon chardev=con,mode=readline -kernel "$NUTTX"
