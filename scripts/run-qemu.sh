#!/usr/bin/env bash
# 在 QEMU 中运行 qemu-armv8a 构建产物（GICv3，单核）
# 用法：scripts/run-qemu.sh [nuttx 路径]
# 退出：Ctrl-A 然后按 x
set -e
NUTTX="${1:-$(dirname "$0")/../src/nuttx/nuttx}"
exec qemu-system-aarch64 -cpu cortex-a53 -nographic \
  -machine virt,virtualization=on,gic-version=3 \
  -net none -chardev stdio,id=con,mux=on -serial chardev:con \
  -mon chardev=con,mode=readline -kernel "$NUTTX"
