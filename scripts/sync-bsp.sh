#!/usr/bin/env bash
# BSP 代码与项目仓库之间的同步。
#
# 代码分两处，都在真实的 git 仓库里，各自以补丁形式归档到 bsp/：
#
#   1. SoC 层 —— nuttx 仓库，分支 rk3576-bsp
#      arch/arm64/{include,src}/rk3576/ + arch/arm64/Kconfig 三处注册
#      按大赛规则不能写进队伍仓库本体，只能以补丁保存。
#
#   2. 板级 —— 本仓库 board/kickpi-k7/，由 manifest 的 <linkfile> 映射到
#      vendor/openvela/boards/contest2026_423_board。
#      比赛期间：fork 本仓 -> PR -> 自行 review 合入。
#      获奖后：整个 board/kickpi-k7/ 原样 PR 到 vendor_rockchip 的
#      boards/rk3576/kickpi-k7/（目录结构已一一对应），
#      vendor 顶层三件套在 bsp/vendor-rockchip-toplevel/。
#
#   ./scripts/sync-bsp.sh export   从两个分支重新生成补丁
#   ./scripts/sync-bsp.sh apply    在一份全新的 repo sync 上还原
#   ./scripts/sync-bsp.sh check    检查补丁是否已落后
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # 队伍仓根
WS="$(cd "$ROOT/.." && pwd)"                              # openvela 工作区根
NUTTX="$WS/nuttx"
# ★ 芯片层已搬到本仓 chip/rk3576/，不再以补丁形式存在。
#
#   《新平台适配指南》要求「所有定制代码存放在 vendor 目录中，不得修改
#   核心代码」，芯片层通过 CONFIG_ARCH_CHIP_CUSTOM_DIR 从树外加载。
#
#   nuttx 仓里现在只剩两类**通用驱动**，按《驱动开发指南》它们本就该在
#   drivers/ 下，将来各自独立提 PR 给上游：
#       drivers/timers/hym8563.c    新增的通用 I2C RTC 驱动
#       drivers/input/ft5x06.c      给已有通用驱动修的一处 bug
#
#   所以导两份：
#       nuttx-drivers.patch   压平的净差异，就是要提上游的内容
#       nuttx-history.patch   完整提交序列，留作开发过程的记录
P_NUTTX="$ROOT/bsp/nuttx-drivers.patch"
P_HIST="$ROOT/bsp/nuttx-history.patch"
BRANCH=rk3576-bsp
UPSTREAM=dev-ai-contest-2026

[ -d "$NUTTX/.git" ] || { echo "找不到 $NUTTX，请先 repo sync"; exit 1; }
mkdir -p "$ROOT/bsp"

# 基线 = 分支与上游 ref 的分叉点。
# 不能用 "$BRANCH^"：分支上一旦有第二个提交，补丁会静默只含最后一个。
base() {  # $1 = repo
  git -C "$1" merge-base "$BRANCH" "refs/remotes/openvela/$UPSTREAM" 2>/dev/null \
    || git -C "$1" merge-base "$BRANCH" "refs/remotes/m/$UPSTREAM" 2>/dev/null
}

do_export() {  # $1=repo $2=patchfile $3=名字
  local B n_p n_b
  B=$(base "$1") || { echo "✗ $3：分支 $BRANCH 不存在"; return 1; }
  # 净差异：直接可提上游的内容
  git -C "$1" diff "$B..$BRANCH" > "$2"
  # 完整序列：开发过程的记录（含已搬走的芯片层）
  git -C "$1" format-patch --stdout "$B..$BRANCH" > "$P_HIST"
  n_p=$(grep -c '^From ' "$P_HIST" || true)
  n_b=$(git -C "$1" rev-list --count "$B..$BRANCH")
  echo "$3"
  echo "  基线     : $B"
  echo "  净差异   -> $(basename "$2")"
  git -C "$1" diff --stat "$B..$BRANCH" | tail -1 | sed 's/^/             /'
  echo "  提交序列 -> $(basename "$P_HIST")（$n_p 个提交）"
  [ "$n_p" = "$n_b" ] || { echo "  ✗ 提交数对不上，记录不完整！"; return 1; }
}

case "${1:-}" in
  export)
    do_export "$NUTTX"  "$P_NUTTX"  "SoC 层 (nuttx)"
    echo "板级代码在本仓 board/kickpi-k7/，随本仓提交，无需补丁"
    ;;
  apply)
    for pair in "$NUTTX:$P_NUTTX"; do
      repo="${pair%%:*}"; patch="${pair#*:}"
      [ -f "$patch" ] || { echo "缺 $patch，跳过"; continue; }
      git -C "$repo" checkout -b "$BRANCH" "openvela/$UPSTREAM" 2>/dev/null \
        || git -C "$repo" checkout "$BRANCH"
      git -C "$repo" am "$patch"
      echo "✓ 已还原 $(basename "$repo")"
    done
    ;;
  check)
    fail=0
    for pair in "$NUTTX:SoC 层"; do
      repo="${pair%%:*}"; name="${pair#*:}"
      cur=$(git -C "$repo" branch --show-current)
      if [ "$cur" != "$BRANCH" ]; then
        echo "⚠ $name：当前在分支 $cur，不是 $BRANCH"; fail=1; continue
      fi
      if [ -z "$(git -C "$repo" status --porcelain)" ]; then
        echo "✓ $name：工作区干净，补丁与分支一致"
      else
        echo "⚠ $name：有未提交改动，补丁已落后。先提交再 export："
        git -C "$repo" status --porcelain | sed 's/^/    /'
        fail=1
      fi
    done
    "$ROOT/scripts/check-addr.sh" || fail=1
    exit $fail
    ;;
  *)
    sed -n '2,25p' "$0"; exit 1 ;;
esac
