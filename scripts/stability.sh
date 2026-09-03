#!/usr/bin/env bash
# xTS 稳定性与启动时间测量（1.2.1-1.2.4、2.1.3、2.1.4）。
#
# 用法：
#   ./scripts/stability.sh reboot [次数]   反复重启，测 Reboot 时间与稳定性
#   ./scripts/stability.sh cold   [次数]   冷启动（经下载模式复位）
#   ./scripts/stability.sh res             采集 RAM / Flash 占用
#
# ★ 启动时间的计法
#
#   从触发复位的那一刻开始计，到串口出现 "NuttShell" 为止。这样量到的
#   是「用户按下电源到能敲命令」的时间，包含引导器，与实际体验一致；
#   只量内核自己那一段会显著偏小，对比时容易失真。
#
# ★ 为什么冷启动要经下载模式
#
#   板上没有可编程的断电开关，rkdeveloptool 的 rd 是最接近上电复位的
#   手段（会重新走一遍引导器）。真正的冷启动需要拔插电源，无法自动化，
#   这一点在结果里注明，不含糊过去。
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WS="$(cd "$ROOT/.." && pwd)"
SERIAL="${SERIAL:-/dev/ttyUSB0}"
BAUD="${BAUD:-1500000}"
RKDEV="${RKDEV:-$HOME/rkdeveloptool/rkdeveloptool}"
USBIPD="${USBIPD:-/mnt/c/Program Files/usbipd-win/usbipd.exe}"
OUT="${OUT:-$ROOT/notes/xts-stability.md}"

stty -F "$SERIAL" "$BAUD" raw -echo -echoe -echok -crtscts

# 送一条命令给 nsh（逐字符，避免行编辑打乱）
send() {
  local c="$1" i
  for ((i=0; i<${#c}; i++)); do
    printf '%s' "${c:$i:1}" > "$SERIAL"
    sleep 0.02
  done
  printf '\r' > "$SERIAL"
}

# 抓串口直到出现 NuttShell 或超时，回显耗时（毫秒）
wait_boot() {
  local log="$1" limit="${2:-30}"
  local t0 t1
  t0=$(date +%s%N)
  timeout "$limit" cat "$SERIAL" > "$log" 2>&1 &
  local p=$!
  while kill -0 $p 2>/dev/null; do
    if grep -qa 'NuttShell' "$log" 2>/dev/null; then
      t1=$(date +%s%N)
      kill $p 2>/dev/null || true
      wait $p 2>/dev/null || true
      echo $(( (t1 - t0) / 1000000 ))
      return 0
    fi
    sleep 0.05
  done
  echo -1
}

attach_usb() {
  local busid
  busid=$("$USBIPD" list 2>/dev/null | awk '/2207:350e/{print $1; exit}')
  [ -n "$busid" ] && "$USBIPD" attach --wsl --busid "$busid" >/dev/null 2>&1 || true
  sleep 2
}

case "${1:-}" in
  reboot)
    n="${2:-10}"
    echo "反复重启 $n 次（软重启，走 loader normal）"
    ok=0; fail=0; times=()
    for ((i=1; i<=n; i++)); do
      log=$(mktemp)
      timeout 40 cat "$SERIAL" > "$log" 2>&1 &
      p=$!
      sleep 0.5
      send "loader normal"
      t0=$(date +%s%N)
      found=0
      for ((w=0; w<400; w++)); do
        if grep -qa 'NuttShell' "$log" 2>/dev/null; then
          t1=$(date +%s%N); found=1; break
        fi
        sleep 0.1
      done
      kill $p 2>/dev/null || true; wait $p 2>/dev/null || true
      if [ "$found" = 1 ]; then
        ms=$(( (t1 - t0) / 1000000 ))
        times+=("$ms"); ok=$((ok+1))
        echo "  第 $i 次: ${ms}ms"
      else
        fail=$((fail+1))
        echo "  第 $i 次: 未启动（超时）"
      fi
      rm -f "$log"
      sleep 1
    done
    echo "结果: 成功 $ok / 失败 $fail"
    if [ ${#times[@]} -gt 0 ]; then
      printf '%s\n' "${times[@]}" | awk '{s+=$1; if(min==""||$1<min)min=$1; if($1>max)max=$1}
        END{printf "启动时间: 平均 %.0fms 最小 %dms 最大 %dms\n", s/NR, min, max}'
    fi
    ;;

  cold)
    n="${2:-5}"
    echo "冷启动 $n 次（经下载模式复位，最接近上电）"
    ok=0; fail=0; times=()
    for ((i=1; i<=n; i++)); do
      send "loader" ; sleep 6
      attach_usb
      log=$(mktemp)
      timeout 40 cat "$SERIAL" > "$log" 2>&1 &
      p=$!
      sleep 0.3
      t0=$(date +%s%N)
      timeout 20 "$RKDEV" rd >/dev/null 2>&1 || true
      found=0
      for ((w=0; w<400; w++)); do
        if grep -qa 'NuttShell' "$log" 2>/dev/null; then
          t1=$(date +%s%N); found=1; break
        fi
        sleep 0.1
      done
      kill $p 2>/dev/null || true; wait $p 2>/dev/null || true
      if [ "$found" = 1 ]; then
        ms=$(( (t1 - t0) / 1000000 ))
        times+=("$ms"); ok=$((ok+1)); echo "  第 $i 次: ${ms}ms"
      else
        fail=$((fail+1)); echo "  第 $i 次: 未启动"
      fi
      rm -f "$log"; sleep 1
    done
    echo "结果: 成功 $ok / 失败 $fail"
    if [ ${#times[@]} -gt 0 ]; then
      printf '%s\n' "${times[@]}" | awk '{s+=$1; if(min==""||$1<min)min=$1; if($1>max)max=$1}
        END{printf "冷启动时间: 平均 %.0fms 最小 %dms 最大 %dms\n", s/NR, min, max}'
    fi
    ;;

  res)
    echo "采集资源占用"
    log=$(mktemp)
    timeout 20 cat "$SERIAL" > "$log" 2>&1 &
    p=$!
    sleep 0.5
    send ""; sleep 1
    send "free"; sleep 2
    send "uname -a"; sleep 2
    kill $p 2>/dev/null || true; wait $p 2>/dev/null || true
    echo "--- 板上 RAM ---"
    tr -d '\r' < "$log" | grep -aA2 'total' | head -4
    rm -f "$log"
    echo "--- 镜像体积（Flash 占用）---"
    ls -l "$WS/nuttx/nuttx.bin" | awk '{printf "nuttx.bin      %d 字节 (%.1f KB)\n", $5, $5/1024}'
    ls -l "$HOME/boot-nuttx.img" 2>/dev/null | awk '{printf "boot-nuttx.img %d 字节 (%.1f KB)\n", $5, $5/1024}'
    ;;

  *)
    sed -n '2,20p' "$0"; exit 1 ;;
esac
