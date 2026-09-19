#!/usr/bin/env bash
# 烧 U-Boot（uboot 分区，LBA 16384）：备份 + 写 + 回读 + 失败回滚。
#
# 用法：板子已在下载模式（先跑 `scripts/flash.sh --stay`）后
#   ./scripts/flash-uboot.sh [uboot.img]      默认 ~/rk3576-amp/u-boot/uboot.img
#   然后 `rkdeveloptool rd` 复位。
#
# ★ 写坏了怎么办：备份在 /tmp/k7-uboot/<时间>/lba16384-before.bin。
#   U-Boot 起不来时 SPL 仍在（idblock 不动），按住 Maskrom 键上电，
#   `rkdeveloptool db <loader> && rkdeveloptool wl 16384 <备份>` 即可恢复。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=../amp/layout.sh
source "$ROOT/amp/layout.sh"
RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
IMG="${1:-$HOME/rk3576-amp/u-boot/uboot.img}"
REGION=$((24576 - AMP_UBOOT_LBA))

# db 下载的引导器跑起来后 ld 仍报 Maskrom，能 rfi 读出 Flash 信息就算可写。
timeout 10 "$RKDEV" rfi 2>/dev/null | grep -q "Flash Size" ||
  { echo "板子不在下载模式（先跑 scripts/flash.sh --stay）"; exit 1; }
PATH="$HOME/rk3576-amp/u-boot/tools:$PATH" mkimage -l "$IMG" | grep -q "Image 0 (uboot)" ||
  { echo "$IMG 不是 U-Boot FIT"; exit 1; }

sectors=$(( ($(stat -c%s "$IMG") + 511) / 512 ))
amp_check_write "$AMP_UBOOT_LBA" "$sectors" uboot

dir=/tmp/k7-uboot/$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$dir"
cp "$IMG" "$dir/uboot-new.img"
"$RKDEV" rl "$AMP_UBOOT_LBA" "$REGION" "$dir/lba16384-before.bin" >/dev/null
echo "备份：$dir/lba16384-before.bin"
sha256sum "$dir/lba16384-before.bin" "$IMG"

rollback() {
  echo "回读不一致，回滚到备份" >&2
  "$RKDEV" wl "$AMP_UBOOT_LBA" "$dir/lba16384-before.bin" >/dev/null
  "$RKDEV" rl "$AMP_UBOOT_LBA" "$REGION" "$dir/rollback-readback.bin" >/dev/null
  cmp "$dir/lba16384-before.bin" "$dir/rollback-readback.bin" &&
    echo "已回滚并校验" >&2
  exit 1
}

"$RKDEV" wl "$AMP_UBOOT_LBA" "$IMG" >/dev/null || rollback
"$RKDEV" rl "$AMP_UBOOT_LBA" "$sectors" "$dir/readback.bin" >/dev/null || rollback
cmp -n "$(stat -c%s "$IMG")" "$IMG" "$dir/readback.bin" || rollback
echo "U-Boot 写入并回读一致（$sectors 扇区）"
