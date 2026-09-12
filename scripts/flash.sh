#!/usr/bin/env bash
# 一键烧写：把 nuttx.bin 打包并写进板子，全程无需碰板子。
#
# ★ 免 recovery 的原理
#
#   Rockchip 引导器支持软件触发下载模式：PMU0_GRF 里有一个跨复位保留的
#   寄存器，写入下载模式魔数后复位，U-Boot 启动时读到就进 rockusb，
#   不必按住 recovery 键。板端由 nsh 命令 loader 完成（见
#   nuttx/arch/arm64/src/rk3576/rk3576_reboot.c）。
#
#     syscon@26024000 (rk3576-pmu0-grf) + 0x40  <- 0x5242C301
#     CRU_GLB_SRST_FST (0x0C08)                 <- 0xfdb9
#
#   两个值都取自原厂 dtb 的 syscon-reboot-mode 节点与 TRM Part1。
#
#   WSL 下 USB 还要转发一次：板子重启会重新枚举，usbipd 的绑定跟着掉，
#   所以每轮都要 attach 一次。usbipd.exe 可以在 WSL 里直接调用。
#
# 用法：
#   ./scripts/flash.sh              打包 + 烧写 + 复位启动
#   ./scripts/flash.sh --no-build   跳过打包，直接烧现有镜像
#   ./scripts/flash.sh --stay       烧完停在下载模式，不复位
#   ./scripts/flash.sh --raw        裸镜像启动（需自编 U-Boot，见 bsp/uboot/）
set -e

# --raw：走自编 U-Boot 的裸镜像启动（路径 B），不套 Android boot.img 壳。
#        需要板上已烧入带新 bootcmd 的 U-Boot（见 bsp/uboot/）。
do_raw=0

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"

NUTTX_BIN="$WS/nuttx/nuttx.bin"
ORIG_IMG="${ORIG_IMG:-$HOME/boot-orig.img}"
BOOT_IMG="${BOOT_IMG:-$HOME/boot-nuttx.img}"
RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
USBIPD="${USBIPD:-/mnt/c/Program Files/usbipd-win/usbipd.exe}"
SERIAL="${SERIAL:-/dev/ttyUSB0}"
BAUD="${BAUD:-115200}"
FLASH_LBA="${FLASH_LBA:-51200}"

do_build=1
do_reset=1
for a in "$@"; do
  case "$a" in
    --no-build) do_build=0 ;;
    --raw)      do_raw=1 ;;
    --stay)     do_reset=0 ;;
    *) echo "未知参数: $a"; exit 1 ;;
  esac
done

# 1) 打包
if [ "$do_raw" = 1 ]; then
  echo "裸镜像模式：boot 分区 = nuttx.bin + board.dtb（无 Android 壳）"
elif [ "$do_build" = 1 ]; then
  [ -f "$NUTTX_BIN" ] || { echo "找不到 $NUTTX_BIN，先编译"; exit 1; }
  python3 "$ROOT/scripts/repack-bootimg.py" "$ORIG_IMG" "$NUTTX_BIN" "$BOOT_IMG" >/dev/null
  echo "已打包 $(stat -c%s "$BOOT_IMG") 字节"
fi

# 2) 让板子进下载模式
in_loader() { timeout 10 "$RKDEV" ld 2>/dev/null | grep -q Loader; }

attach_usb() {
  local busid
  busid=$("$USBIPD" list 2>/dev/null | awk '/2207:350e/{print $1; exit}')
  [ -n "$busid" ] || return 1
  "$USBIPD" attach --wsl --busid "$busid" >/dev/null 2>&1 || true
  sleep 2
}

if ! in_loader; then
  attach_usb || true
fi

if ! in_loader; then
  # 板子还在跑 NuttX，用串口叫它自己重启进下载模式
  if [ -w "$SERIAL" ]; then
    echo "通过串口触发下载模式…"
    stty -F "$SERIAL" "$BAUD" raw -echo -echoe -echok -crtscts
    # ★ 前台如果跑着 ai_agent，它有自己的 vela> 提示符，会把 loader 当成
    #   未知命令吃掉 —— 板子根本不会进下载模式，而失败要到 rkdeveloptool
    #   找不到设备时才暴露，方向很容易查偏。
    #
    #   这里只**报告**不代劳：试过在这里替用户发 quit，结果 agent 退出时
    #   把控制台一起带走了，板子既没进下载模式、串口也没了回显，反而从
    #   "重发一次就好"变成"必须按 RESET"。自动化在不确定的前台状态上
    #   动手，代价比它省下的那一步大。
    printf 'loader\r' > "$SERIAL"
    sleep 6
    attach_usb || true
  fi
fi

if ! in_loader; then
  echo "板子未进入下载模式。可能原因："
  echo "  - 串口没在 nsh 提示符下（先确认 $SERIAL 能敲命令）"
  echo "  - 前台跑着 ai_agent（提示符是 vela> 而不是 nsh>）：先在它里面敲 quit
  - 板上跑着原厂 Android（提示符是 console:/ \$）：在它里面执行 reboot loader
  - 板上固件还没有 loader 命令（首次需手动 recovery 烧一次）"
  echo "  - usbipd 未共享设备：在 Windows 管理员终端执行"
  echo "      usbipd bind --force --busid <busid>"
  exit 1
fi

# 3) 烧写
echo "烧写中…"
if [ "$do_raw" = 1 ]; then
  # 裸镜像：内核在分区起始，dtb 在分区内偏移 4MB（LBA +0x2000）
  #
  # ★ dtb 只是喂给 U-Boot 的 —— arm64 的 booti 第三个参数给 '-' 时它仍会
  #   去解析 FDT，实测会在 U-Boot 自己身上 Data Abort。NuttX 不读设备树。
  # ★ 用我们自己的 344 字节最小 FDT，不是原厂那份 264KB 的 board.dtb。
  #
  #   NuttX 根本不读设备树（arm64_head.S 里 x0 进 real_start 就被
  #   switch_el 覆盖了），这份 FDT 纯粹是让 booti 不崩。既然只要"结构
  #   合法"，就不该让启动链依赖一个从原厂固件里抠出来的二进制。
  #   源码在 board/kickpi-k7/scripts/booti-stub.dts，已上板验证。
  DTB="${DTB:-$ROOT/board/kickpi-k7/scripts/booti-stub.dtb}"
  [ -f "$DTB" ] || { echo "找不到 $DTB（裸镜像模式需要一份 FDT 喂给 booti）"; exit 1; }
  timeout 300 "$RKDEV" wl "$FLASH_LBA" "$NUTTX_BIN" 2>&1 | tail -1
  timeout 300 "$RKDEV" wl $((FLASH_LBA + 0x2000)) "$DTB" 2>&1 | tail -1
else
  timeout 300 "$RKDEV" wl "$FLASH_LBA" "$BOOT_IMG" 2>&1 | tail -1
fi

# 4) 启动
if [ "$do_reset" = 1 ]; then
  timeout 20 "$RKDEV" rd 2>&1 | tail -1
  echo "已复位启动"
else
  echo "停在下载模式（--stay）"
fi
