#!/usr/bin/env bash
# 用法：source scripts/env.sh
#
# openvela 自带官方环境脚本 src/build/envsetup.sh，它负责：
#   - 把 prebuilts/gcc/.../aarch64-none-elf/bin 加进 PATH
#   - 把 prebuilts/tools/python/bin 加进 PATH（menuconfig / olddefconfig）
#   - 组装 PYTHONPATH（kconfiglib、pyelftools、Mako 等）
#   - 载入各 vendor 的 vendorsetup.sh
#
# 顶层 build.sh 内部也是 source 它（build.sh:233）。直接用官方的，
# 不要自己拼 PATH —— 手工拼很容易漏掉 PYTHONPATH，
# 表现为 olddefconfig 报 ModuleNotFoundError，或退回 kconfig-conf 后
# 报 `unknown option "osource"`。

# 本仓库是大赛专属仓 contest2026_423_nanshannan/，工作区根是上一级。
# 该布局是大赛日志工具的采集前提：它从 git 仓库根向上寻找 .repo/，
# 找不到就完全不采集。详见 RECON.md A8。
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

pushd "$SRC" >/dev/null || return 1
source ./build/envsetup.sh
popd >/dev/null || return 1

echo
echo "menuconfig(kconfiglib): $(command -v menuconfig || echo '未找到')"
echo "aarch64-none-elf-gcc  : $(command -v aarch64-none-elf-gcc || echo '未找到')"
echo
echo "编译（板级在 vendor 下，用 configure.sh 的路径形式）："
echo "  cd $SRC/nuttx"
echo "  ./tools/configure.sh -e ../vendor/openvela/boards/contest2026_423_board/configs/nsh"
echo "  make -j4"
echo
echo "参考环境（板子到位前）： cd $SRC/nuttx && ./tools/configure.sh -e qemu-armv8a:nsh_gicv2 && make -j4"
echo "  ^ nsh_gicv2 是 GICv2 配置，RK3576 用的就是 GICv2，先在这上面跑熟"
echo "换配置前先： cd $SRC/nuttx && make distclean"
echo "★ 切分支后若 distclean 报 chip/Make.defs 缺失，先删悬空软链接："
echo "  rm -f arch/arm64/src/{chip,board} include/arch/{chip,board} .config Make.defs"
