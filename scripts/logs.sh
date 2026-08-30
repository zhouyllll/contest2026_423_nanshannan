#!/usr/bin/env bash
# AI Coding 日志：查看 / 导出 / 提交
#
# 大赛要求把与 AI 的对话日志放进本仓 logs/<github_login>/，随代码一起提交。
#
# 采集机制（官方 contest-log-collector）：
#   * 门控：当前目录向上能找到 .repo/ 才采集。
#     ★ 必须在本仓或其子目录里启动 Claude Code，否则整场对话不会被记录。
#   * 会话结束（Stop / SessionEnd hook）自动写入 staging 和本仓 logs/。
#   * 工具永远不会 git push，提交由你控制。
#
#   ./scripts/logs.sh status         看本仓 logs/ 现状
#   ./scripts/logs.sh list           看本机 staging 里有哪些会话
#   ./scripts/logs.sh add <sid>      把某个会话导入本仓 logs/
#   ./scripts/logs.sh today          把今天的会话导入本仓 logs/
#   ./scripts/logs.sh commit         git add logs/ + commit（不 push）
set -eu

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
EXPORT="$WS/.claude/skills/contest-log-collector/tools/export-session.py"
LOGIN="$(grep -oP '^GITHUB_LOGIN=\K.*' "$HOME/.claude/contest-collector.env" 2>/dev/null || echo '')"

[ -f "$EXPORT" ] || { echo "找不到 $EXPORT，先 repo sync"; exit 1; }
[ -n "$LOGIN" ]  || { echo "~/.claude/contest-collector.env 里没有 GITHUB_LOGIN"; exit 1; }

case "${1:-status}" in
  status)
    D="$ROOT/logs/$LOGIN"
    echo "本仓 logs/$LOGIN/"
    if [ -d "$D" ]; then
      find "$D" -name '*.jsonl' | sed "s|$ROOT/||;s|^|  |" | sort
      echo "  ---"
      echo "  会话数: $(find "$D" -name '*.jsonl' | wc -l)   大小: $(du -sh "$D" | cut -f1)"
    else
      echo "  （空）"
    fi
    ;;
  list)
    cd "$ROOT" && python3 "$EXPORT" --list
    ;;
  add)
    [ $# -ge 2 ] || { echo "用法: $0 add <session-id>"; exit 1; }
    cd "$ROOT" && python3 "$EXPORT" --session "$2" --confirm
    ;;
  today)
    cd "$ROOT" && python3 "$EXPORT" --today --confirm
    ;;
  commit)
    cd "$ROOT"
    git add logs/
    git commit -s -m "logs: capture session $(date +%F)" || echo "（无新增日志）"
    echo "已提交到本地，push 由你自己执行"
    ;;
  *)
    sed -n '2,22p' "$0"; exit 1 ;;
esac

# ⚠️ 不要用 --all 或 --backfill 一把梭：staging 里还留着大量与本项目无关的
#    会话（求职、Agent、embedded-lab 等），全导进来既臃肿又会泄露无关内容。
#    只导本项目的会话。
