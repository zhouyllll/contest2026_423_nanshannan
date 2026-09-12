#!/usr/bin/env bash
# 做一张 KICKPI-K7 的 AMP 启动 SD 卡镜像。
#
# ★ 为什么走 SD 而不是继续往 eMMC 里灌
#
#   rkdeveloptool 这条 USB 通道**写不到 32MB 以上**（LBA >= 0x10000 的写
#   静默丢弃：命令报 100%，回读是 0xCC）。已经逐项验证过：
#     - LBA 65528 写入后读回一致；LBA 70000 写入后读回全 0xCC
#     - 换 rkbin 的 spl_loader 用 db 重下，一样
#     - 用 wlx <分区名> 走分区路径，一样
#   而 AMP 要往 boot 分区（LBA 51200 = 25MB）放一个 46MB 的 Linux 镜像，
#   尾巴必然越过 32MB。
#
#   SD 卡绕开了整条 USB 通道：主机直接 dd 整张卡，没有任何长度限制。
#   顺带还有两个好处 —— eMMC 一个字节都不用动，现在能跑的 NuttX 原样
#   留着；出问题拔卡就回到原状，连 maskrom 都不用进。
#
#   SDK 文档《Rockchip_Developer_Guide_SD_Boot_CN》第 5.2 节明写：
#   「SPL 流程则是设置 SD 卡为最高优先级的启动设备，如果 SD 卡有可以
#   启动的固件，则优先从 sd 卡加载固件并启动。」
#
# ★ 布局（与 eMMC 上那张 GPT 的偏移一致，这样 SPL/U-Boot 按分区名找得到）
#
#     LBA     64   idblock   SPL + DDR 初始化（BootROM 认的就是这个）
#     LBA  16384   uboot     带 AMP 与 bootamp 命令的 U-Boot
#     LBA  51200   boot      Linux(A72) 的 Android 镜像 + 只含 A72 的 DTB
#     LBA 182272   amp       AMP FIT，里面是 openvela(A53)
#
#   idblock 落在 GPT 的第一个可用扇区之前（保留区），所以 GPT 从 LBA 2048
#   之后才开始分区，不会打架。
#
# 用法: ./mk-sdcard.sh <输出镜像> [镜像大小MB，默认 256]
set -e

OUT="${1:?用法: mk-sdcard.sh <输出镜像> [大小MB]}"
SIZE_MB="${2:-256}"

A="$HOME/rk3576-amp"
IDB="$A/u-boot/rk3576_idblock_v1.09.108.img"
UBOOT="$A/out/uboot-amp.img"
BOOT="$A/out/boot-linux-amp.img"
AMP="$A/out/amp.itb"

for f in "$IDB" "$UBOOT" "$BOOT" "$AMP"; do
  [ -f "$f" ] || { echo "缺 $f"; exit 1; }
done

# 空镜像。用 truncate 而不是 dd if=/dev/zero：稀疏文件，快且不占盘。
rm -f "$OUT"
truncate -s "${SIZE_MB}M" "$OUT"

# GPT：分区名与偏移照抄 eMMC 那张，只保留 AMP 用得到的四个
# （security/trust/misc/dtbo 等这张卡上不需要，留空不影响启动）。
sgdisk --zap-all "$OUT" >/dev/null 2>&1 || true
sgdisk \
  -n 1:16384:24575   -c 1:uboot \
  -n 2:51200:182271  -c 2:boot \
  -n 3:182272:-34    -c 3:amp \
  "$OUT" >/dev/null

# idblock 在 GPT 管不到的保留区（扇区 64 起），直接按偏移写。
dd if="$IDB"   of="$OUT" bs=512 seek=64     conv=notrunc status=none
dd if="$UBOOT" of="$OUT" bs=512 seek=16384  conv=notrunc status=none
dd if="$BOOT"  of="$OUT" bs=512 seek=51200  conv=notrunc status=none
dd if="$AMP"   of="$OUT" bs=512 seek=182272 conv=notrunc status=none

echo "✓ $OUT  ($(du -h "$OUT" | cut -f1)，稀疏)"
sgdisk -p "$OUT" | tail -6
