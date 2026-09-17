#!/usr/bin/env bash
# 一键烧写（AMP 布局）：把 nuttx.bin 打成 FIT，写进 trust 分区，复位启动。
#
# 用法：
#   ./scripts/flash.sh                打包双系统 FIT（openvela + Linux）+ 烧写 + 复位
#   ./scripts/flash.sh --solo         打包单系统 FIT（不带 Linux）
#   ./scripts/flash.sh --fit X.itb    不打包，直接烧现成的 FIT
#   ./scripts/flash.sh --stay         烧完停在下载模式，不复位
#
# ★ 只写 FIT 区（amp/layout.sh 的 AMP_FIT_LBA）。
#
#   旧版按单系统布局往 LBA 51200 写 boot.img / 裸 nuttx.bin。AMP 布局下
#   那里是 Linux 内核（49152 起 7.2MB），2026-09-17 就是这样把内核写坏的：
#   双系统每次都卡死在 NSH 横幅附近，单系统却一切正常。旧的 boot.img /
#   --raw 路径因此删掉了；写盘一律经过 amp_check_write。
#
# ★ 进下载模式的两条路
#
#   1. 板子在 nsh> 下：发 `loader`（PMU0_GRF 写下载模式魔数后复位）。
#   2. 不行就发 `reboot`，在 U-Boot 的 1 秒倒计时里连发 Ctrl-C，
#      然后 `rockusb 0 mmc 0`。板子已经挂死时，按一下 RESET 也走这条。
#
#   前台如果是 ai_agent（提示符 vela>），loader 会被它吃掉；这里不替用户
#   发 quit —— 试过，agent 退出时把控制台一起带走了，反而只能按 RESET。
#
#   WSL 下 USB 还要转发一次：进 rockusb 后设备重新枚举，每轮都要 attach。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
# shellcheck source=../amp/layout.sh
source "$ROOT/amp/layout.sh"

NUTTX_BIN="${NUTTX_BIN:-$WS/nuttx/nuttx.bin}"
RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
USBIPD="${USBIPD:-/mnt/c/Program Files/usbipd-win/usbipd.exe}"
SERIAL="${SERIAL:-/dev/ttyUSB0}"
BAUD="${BAUD:-1500000}"
MKIMAGE_PATH="${MKIMAGE_PATH:-$HOME/rk3576-amp/u-boot/tools:$HOME/rk3576-amp/u-boot/scripts/dtc}"
OUT="${OUT:-/tmp/k7-amp-fit/build}"

its=amp.its
fit=""
do_reset=1
while (( $# )); do
  case "$1" in
    --solo) its=amp-solo.its ;;
    --fit)  fit="$2"; shift ;;
    --stay) do_reset=0 ;;
    *) echo "未知参数: $1"; exit 1 ;;
  esac
  shift
done

# 1) 打包
if [[ -z "$fit" ]]; then
  [[ -f "$NUTTX_BIN" ]] || { echo "找不到 $NUTTX_BIN，先编译"; exit 1; }
  mkdir -p "$OUT"
  cp "$NUTTX_BIN" "$OUT/openvela-amp.bin"
  cp "$ROOT/amp/fit/$its" "$OUT/"
  (cd "$OUT" && PATH="$MKIMAGE_PATH:$PATH" \
     mkimage -f "$its" -E -p 0xe00 "${its%.its}.itb" >/dev/null)
  fit="$OUT/${its%.its}.itb"
  echo "已打包 $fit（$(( $(stat -c%s "$fit") / 512 )) 扇区）"
fi
K7_FIT="$fit" bash "$ROOT/scripts/flash-xts-netsh-fit.sh" --prepare >/dev/null

# 2) 进下载模式
# Loader 或 Maskrom 都算：`loader` 命令实测会落到 Maskrom，
# 后面的 FIT 脚本会先 `db` 下载引导器再写。
in_loader() { timeout 10 "$RKDEV" ld 2>/dev/null | grep -qE 'Loader|Maskrom'; }

# 设备重新枚举要几秒，第一次 attach 常常落空（usbipd 显示 Shared 而不是
# Attached）。一直重试到 rkdeveloptool 看得见为止。
attach_usb() {
  local busid i
  for i in $(seq 1 20); do
    busid=$("$USBIPD" list 2>/dev/null | tr -d '\r' | awk '/2207:/ && !/Attached/ {print $1; exit}')
    if [[ -n "$busid" ]]; then
      "$USBIPD" attach --wsl --busid "$busid" >/dev/null 2>&1 || true
      sleep 2
    fi
    in_loader && return 0
    sleep 1
  done
  return 1
}

# 发一条命令，然后立刻盯串口：一看到 U-Boot 就连发 Ctrl-C 打断，
# 再发 `rockusb 0 mmc 0`。
#   $1 = 先发给 nsh 的命令（可空）  $2 = 最多等几秒  $3 = 提示语（可空）
#
# ★ 发完命令必须马上开始盯。U-Boot 的倒计时只有 1 秒，而 `loader`
#   在不同镜像上表现不一：有的落到 Maskrom（串口上不会出现 U-Boot，
#   等满超时即可），有的只是普通重启 —— 旧版先 sleep 6 再去抓，
#   U-Boot 早就过去了。
serial_to_rockusb() {
  python3 - "$SERIAL" "$BAUD" "$1" "$2" "${3:-}" <<'PY'
import serial, sys, time
s = serial.Serial(sys.argv[1], int(sys.argv[2]), timeout=0.05)
s.reset_input_buffer()
if sys.argv[3]:
    s.write(sys.argv[3].encode() + b'\r')
if sys.argv[5]:
    print(sys.argv[5], flush=True)
buf = b''; seen = False; t0 = time.time()
while time.time() - t0 < float(sys.argv[4]):
    buf = (buf + s.read(4096))[-4000:]
    if not seen and (b'U-Boot' in buf or b'Hit key' in buf):
        seen = True
    if seen:
        s.write(b'\x03')
        if buf.rstrip().endswith(b'=>'):
            time.sleep(0.5); s.read(4096)
            s.write(b'rockusb 0 mmc 0\r'); time.sleep(2)
            sys.exit(0)
sys.exit(1)
PY
}

if ! in_loader; then
  [[ -w "$SERIAL" ]] || { echo "串口 $SERIAL 不可写"; exit 1; }
  # loader 落到 Maskrom 时串口上等不到 U-Boot，20 秒后去 USB 上找
  serial_to_rockusb loader 20 || true
  attach_usb || true
fi
if ! in_loader; then
  serial_to_rockusb reboot 120 "等 U-Boot（板子挂死的话现在按 RESET）…" || true
  attach_usb || true
fi
in_loader || { echo "板子未进入下载模式（usbipd 未共享时：管理员终端 usbipd bind --force --busid <id>）"; exit 1; }

# 3) 烧写（备份 + 写 + 回读 + 失败回滚）
K7_FIT="$fit" bash "$ROOT/scripts/flash-xts-netsh-fit.sh"

# 4) 启动
if (( do_reset )); then
  timeout 20 "$RKDEV" rd 2>&1 | tail -1
  echo "已复位启动"
else
  echo "停在下载模式（--stay）"
fi
