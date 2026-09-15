#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Update only the K7 AMP FIT region after backing it up and verifying reads.

set -euo pipefail

rkdev=${RKDEV:-/home/dministrator/rkdeveloptool/rkdeveloptool}
fit=${K7_NETSH_FIT:-/tmp/k7-netsh-fit/amp.itb}
maskrom_loader=${K7_MASKROM_LOADER:-/home/dministrator/rk3576-uboot/u-boot/rk3576_spl_loader_v1.09.108.bin}
fit_lba=8192
region_sectors=6144
dtb_lba=14336

[[ -f "$fit" ]] || { echo "missing FIT: $fit" >&2; exit 1; }
fit_bytes=$(stat -c %s "$fit")
(( fit_bytes > 0 && fit_bytes % 512 == 0 )) || {
  echo "FIT must have a whole number of 512-byte sectors" >&2
  exit 1
}
fit_sectors=$(( fit_bytes / 512 ))
(( fit_sectors < region_sectors )) || {
  echo "FIT overlaps Linux DTB at LBA $dtb_lba" >&2
  exit 1
}
python3 - "$fit" <<'PY'
from pathlib import Path
import sys
if Path(sys.argv[1]).read_bytes()[:4] != bytes.fromhex('d00dfeed'):
    raise SystemExit('new image has no FIT header; refusing write')
PY

echo "FIT: $fit"
echo "FIT bytes: $fit_bytes; LBA $fit_lba..$(( fit_lba + fit_sectors - 1 )); DTB LBA $dtb_lba"
sha256sum "$fit"

if [[ ${1:-} == --prepare ]]; then
  exit 0
fi

[[ -x "$rkdev" ]] || { echo "missing rkdeveloptool: $rkdev" >&2; exit 1; }
state=$("$rkdev" ld)
if [[ "$state" == *Maskrom* ]]; then
  [[ -f "$maskrom_loader" ]] || { echo "missing MASKROM loader" >&2; exit 1; }
  "$rkdev" db "$maskrom_loader"
  sleep 3
  state=$("$rkdev" ld)
fi
[[ "$state" == *Loader* ]] || {
  echo "RockUSB Loader not available: $state" >&2
  exit 1
}

run_dir="/tmp/k7-netsh-fit/flash-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$run_dir"
backup="$run_dir/lba8192-6144-before.bin"
readback="$run_dir/lba8192-new-readback.bin"
rollback_readback="$run_dir/lba8192-6144-rollback.bin"

echo "Reading current AMP FIT region to $backup"
"$rkdev" rl "$fit_lba" "$region_sectors" "$backup"
[[ $(stat -c %s "$backup") -eq $(( region_sectors * 512 )) ]] || {
  echo "backup has wrong length; refusing write" >&2
  exit 1
}
python3 - "$backup" <<'PY'
from pathlib import Path
import sys
header = Path(sys.argv[1]).read_bytes()[:4]
if header != bytes.fromhex('d00dfeed'):
    raise SystemExit('current FIT header is invalid; refusing write')
print('current FIT header verified')
PY
sha256sum "$backup"

rollback() {
  echo "New FIT verification failed; restoring backed-up region" >&2
  "$rkdev" wl "$fit_lba" "$backup"
  "$rkdev" rl "$fit_lba" "$region_sectors" "$rollback_readback"
  cmp "$backup" "$rollback_readback"
  echo "Backed-up AMP region restored and verified" >&2
}

echo "Writing new FIT"
if ! "$rkdev" wl "$fit_lba" "$fit"; then
  rollback
  exit 1
fi
echo "Reading back new FIT"
if ! "$rkdev" rl "$fit_lba" "$fit_sectors" "$readback" ||
   ! cmp "$fit" "$readback"; then
  rollback
  exit 1
fi
sha256sum "$readback"
echo "FIT write verified. Current region backup: $backup"
echo "Board remains in Loader mode; reset and bootamp only after review."
