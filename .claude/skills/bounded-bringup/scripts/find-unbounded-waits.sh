#!/usr/bin/env bash
# 扫描 C 源码里"可能永远不返回"的等待，供 bring-up 前自查。
#
# 为什么需要它
# ------------
# 在只有一根串口线的板子上，一次永久阻塞就等于一次物理复位。这类写法
# 在代码里长得很无害 —— sem_wait、while(!(reg & BIT))、mq_receive ——
# 单看每一处都"就等一下"，问题只在**没有任何东西保证它会结束**。
#
# 本脚本不做语义分析，只做模式匹配：它会有误报（外层可能已有超时），
# 也会漏报（自制的等待循环千奇百怪）。把它当**清单生成器**，
# 逐条人工确认，不要当判决。
#
# 用法：
#   ./find-unbounded-waits.sh <目录> [更多目录...]
#   ./find-unbounded-waits.sh chip/ board/ app/
set -uo pipefail

DIRS=("$@")
if [ ${#DIRS[@]} -eq 0 ]; then
  echo "用法: $0 <目录> [更多目录...]" >&2
  exit 1
fi

FILES=$(find "${DIRS[@]}" -name '*.c' -o -name '*.h' 2>/dev/null)
[ -z "$FILES" ] && { echo "没找到 .c/.h 文件" >&2; exit 1; }

hit() { printf '\n=== %s ===\n' "$1"; }
n_total=0

report() {   # report <标题> <grep 模式> <说明>
  local title="$1" pat="$2" note="$3" out
  out=$(echo "$FILES" | xargs grep -nE "$pat" 2>/dev/null | grep -v '^\s*\*' || true)
  if [ -n "$out" ]; then
    hit "$title"
    echo "  $note"
    echo
    echo "$out" | sed 's/^/  /'
    n_total=$((n_total + $(echo "$out" | wc -l)))
  fi
}

report "无超时的信号量/互斥等待" \
  '\b(sem_wait|nxsem_wait|pthread_mutex_lock|nxmutex_lock)[[:space:]]*\(' \
  "这些会一直等。确认外层有超时，或改用 *_timedwait / *_tickwait。"

report "无超时的消息队列接收" \
  '\b(mq_receive|nxmq_receive|file_mq_receive)[[:space:]]*\(' \
  "改用 mq_timedreceive。注意：单次超时不等于总量有界，外层仍要有 deadline。"

report "轮询硬件位、循环条件里没有计数器" \
  'while[[:space:]]*\([^;]*(getreg|readl|REG_|_getreg)[^;]*\)[[:space:]]*(;|\{)' \
  "典型的 while (!(getreg32(X) & BIT)); —— 位不置就永远转。要带预算。"

report "for/while(1) 里的忙等延时" \
  '\b(up_udelay|udelay|up_mdelay|nxsig_usleep)[[:space:]]*\(' \
  "逐个看它在不在循环里。★ 高优先级线程里的长时间忙等会饿死系统，
  现象和崩溃一模一样（串口全哑、Ctrl-C 无响应）。"

report "可能被编译期关掉的诊断宏" \
  '\b(dmaerr|auderr|audwarn|audinfo|wlerr|wlinfo|i2cerr|spierr|mtderr|nferr)[[:space:]]*\(' \
  "CONFIG_DEBUG_* 没开时这些展开成空。初始化/探针/失败路径请用 syslog()，
  否则你会在等一条根本不可能出现的信息。"

report "无限循环" \
  '\b(while[[:space:]]*\([[:space:]]*1[[:space:]]*\)|for[[:space:]]*\([[:space:]]*;[[:space:]]*;[[:space:]]*\))' \
  "确认循环体内有能跳出的条件，且该条件一定会成立。"

echo
echo "================================================================"
if [ "$n_total" -eq 0 ]; then
  echo "没有命中。注意这不等于安全 —— 自制的等待循环本脚本认不出来。"
else
  echo "共 $n_total 处待人工确认。"
  echo
  echo "逐条问三个问题："
  echo "  1) 这次等待有上界吗？"
  echo "  2) **整个操作**有上界吗？（逐次有界 != 总量有界）"
  echo "  3) 超时后报告的是**观测到的数值**，还是只有一句\"失败\"？"
fi
