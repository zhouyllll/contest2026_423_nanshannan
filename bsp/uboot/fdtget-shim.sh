#!/bin/sh
# 最小 fdtget 垫片。
#
# ★ 只覆盖一个已知用途：fit-core.sh 第 632 行用 `fdtget -ti <itb> / version`
#   读一个**纯装饰性**的版本号，读不到时脚本自己会跳过（判空）。
#
#   fdtget/fdtput 的其余用途全在**签名分支**里（fit-core.sh 284-347）。
#   本项目不签名（启动日志为 OK_NOT_SIGNED），那条路不会走到。
#   万一走到了，这里**大声失败**而不是返回空 —— 一个悄悄返回错误答案的
#   垫片，比没有垫片更危险。
case "$*" in
  "-ti "*" / version") exit 0 ;;                 # 返回空，脚本判空跳过
  *) echo "fdtget 垫片：未覆盖的用法 [$*]，请安装 device-tree-compiler" >&2
     exit 1 ;;
esac
