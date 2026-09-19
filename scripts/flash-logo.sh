#!/usr/bin/env bash
# 烧开机 logo 的 resource 镜像（dtbo 分区，LBA 40960）：备份 + 写 + 回读 + 失败回滚。
#
# 用法：板子已在下载模式（先跑 `scripts/flash.sh --stay`）后
#   ./scripts/flash-logo.sh [resource.img]   默认 ~/rk3576-amp/out/logo/resource-logo.img
#   （由 scripts/gen-boot-logo.py 生成）
# U-Boot 要带 amp/uboot/0011 才会去 dtbo 分区找 logo。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../amp/layout.sh
source "$ROOT/amp/layout.sh"
RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
IMG="${1:-$HOME/rk3576-amp/out/logo/resource-logo.img}"
REGION=$((AMP_KERNEL_LBA - AMP_LOGO_LBA))

timeout 10 "$RKDEV" rfi 2>/dev/null | grep -q "Flash Size" ||
  { echo "板子不在下载模式（先跑 scripts/flash.sh --stay）"; exit 1; }
[[ "$(head -c 4 "$IMG")" == RSCE ]] || { echo "$IMG 不是 resource 镜像"; exit 1; }

sectors=$(( ($(stat -c%s "$IMG") + 511) / 512 ))
amp_check_write "$AMP_LOGO_LBA" "$sectors" logo

dir=/tmp/k7-logo/$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$dir"
cp "$IMG" "$dir/resource-new.img"
"$RKDEV" rl "$AMP_LOGO_LBA" "$REGION" "$dir/lba40960-before.bin" >/dev/null
echo "备份：$dir/lba40960-before.bin"

rollback() {
  echo "回读不一致，回滚到备份" >&2
  "$RKDEV" wl "$AMP_LOGO_LBA" "$dir/lba40960-before.bin" >/dev/null
  exit 1
}

"$RKDEV" wl "$AMP_LOGO_LBA" "$IMG" >/dev/null || rollback
"$RKDEV" rl "$AMP_LOGO_LBA" "$sectors" "$dir/readback.bin" >/dev/null || rollback
cmp -n "$(stat -c%s "$IMG")" "$IMG" "$dir/readback.bin" || rollback
echo "logo 写入并回读一致（$sectors 扇区）"
