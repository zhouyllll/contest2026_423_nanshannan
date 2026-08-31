#!/usr/bin/env bash
# 归集本项目的 AI Coding 日志到 logs/。
#
# 为什么需要这个脚本
# ------------------
# 官方的 contest-log-collector 用"当前目录向上能找到 .repo/"作隐私门控，
# 会话结束时自动入仓。但从工作区**外**启动的会话（例如 cwd 是
# /home/dministrator/linux）不会被采集 —— 本项目的主力会话正是这种情况。
#
# 官方给的补救手段是 export-session.py --backfill，但它扫描整个
# ~/.claude/projects/，会把**所有**个人项目的对话一并导入。参赛仓默认
# 是 public，那样等于公开无关的私人对话。实测一次 --backfill 会带进
# 39 个无关会话共 23.8MB（论文排版、面试题整理、其它项目……）。
#
# 所以这里改用**会话白名单**：只导出确属本项目的会话，其余一律不碰。
#
# 用法
# ----
#   ./scripts/collect-logs.sh            # 导出并校验
#   ./scripts/collect-logs.sh --check    # 只校验，不导出
#
# 新增会话时，把 session id 追加到下面的 SESSIONS 里。
# 查 id：python3 <skill>/tools/export-session.py --list
#        或看 ~/.claude/projects/<项目目录>/<sid>.jsonl 的文件名。
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
TOOLS="$WS/.claude/skills/contest-log-collector/tools"

# ★ 白名单：只有这里列出的会话会被导出
SESSIONS=(
  # RK3576 / KICKPI-K7 适配主力会话（cwd 在工作区外，不会被自动采集）
  6396523f-2861-4bec-8d27-a68bbd63cfc3
  # openvela-rk3568 前身项目
  0b612fdc-dabd-4706-b76b-9ef79306f36c
)

[ -f "$TOOLS/export-session.py" ] || {
  echo "找不到 $TOOLS/export-session.py —— 是否已 repo sync？" >&2; exit 1; }

if [ "${1:-}" != "--check" ]; then
  for sid in "${SESSIONS[@]}"; do
    python3 "$TOOLS/export-session.py" --session "$sid" --dest "$ROOT" --confirm \
      | sed "s/^/  [${sid:0:8}] /"
  done
fi

python3 "$TOOLS/validate-log.py" "$ROOT/logs/zhouyllll"

echo
echo "提交： git add logs/ && git commit -s -m 'logs: 更新 AI Coding 日志'"
