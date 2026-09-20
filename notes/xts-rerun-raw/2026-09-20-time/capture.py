#!/usr/bin/env python3
"""xTS 1.3.14 时间一致性：设时后静置 24h，按 6/12/18/24h 比对板子与主机时间。

用法：python3 capture.py <输出目录> [总时长秒，默认 24h+5min]

原文步骤：设备与 PC 对时 → 每 6h 检查一次 date（共 4 次）→ 静置 24h 以上，
板子与 PC 的时间差 ≤ 2s。前提是未配网、无 NTP 影响。

与原文的两处差别（都写进结果里）：
- 取样走网络 NSH（192.168.1.50:2323），不走 minicom 时间戳：上一轮（09-14）
  串口在 15h 左右断连，整轮作废。串口这次空着，可以并行用 minicom。
- 每 10 分钟多采一次，6/12/18/24h 四个点照常单独标出。中途中断也有部分证据。

★ 怎么做到亚秒精度
  `date -u +%s` 只有整秒。每次取样连续轮询，**捕捉板子秒数跳变的瞬间**：
  跳变时板子正好走到整秒边界，此刻的主机时间即可与之对齐，
  偏差 = 板子整秒值 − 主机时刻。没捕捉到跳变时退化为整秒比较（记 coarse）。
"""

import importlib.util
import json
import re
import socket
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parents[3] / 'scripts'
spec = importlib.util.spec_from_file_location('netsh', HERE / 'xts-netsh.py')
netsh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netsh)

OUT = Path(sys.argv[1])
TOTAL = float(sys.argv[2]) if len(sys.argv) > 2 else 24 * 3600 + 300
STEP = 600.0
CHECKPOINTS = [6 * 3600, 12 * 3600, 18 * 3600, 24 * 3600]
OUT.mkdir(parents=True, exist_ok=True)


def run(cmd, timeout=20):
    """跑一条 NSH 命令，返回 (输出, 发送时刻, 收到时刻)，主机时刻用 time.time()。"""
    filt = netsh.TelnetFilter()
    with socket.create_connection((netsh.HOST, netsh.PORT), timeout=8) as c, \
            open(OUT / 'session.raw', 'ab') as raw:
        c.settimeout(0.1)
        greet = netsh.receive(c, filt, raw, 10)
        if not netsh.PROMPT.search(greet[-400:]):
            raise RuntimeError('no prompt')
        t0 = time.time()
        c.sendall(cmd.encode() + b'\r\n')
        out = netsh.receive(c, filt, raw, timeout)
        return out.decode('utf-8', 'replace'), t0, time.time()


def board_epoch(text):
    m = re.findall(r'\b(1[0-9]{9})\b', text)
    return int(m[-1]) if m else None


def sample(tag):
    """轮询捕捉板子秒数跳变，算出偏差（板子 − 主机）。"""
    prev = None
    for _ in range(40):                      # 最多约 10s
        text, t0, t1 = run('date -u +%s')
        cur = board_epoch(text)
        if cur is None:
            time.sleep(0.2)
            continue
        if prev is not None and cur != prev[0]:
            # 跳变发生在 prev 那次收到之后、这次发出之前；取中点当作板子的整秒时刻
            host_at_tick = (prev[1] + t0) / 2
            return {'tag': tag, 'mode': 'edge', 'board_epoch': cur,
                    'host_at_board_tick': host_at_tick,
                    'offset_s': round(cur - host_at_tick, 3),
                    'edge_window_s': round(t0 - prev[1], 3),
                    'wall': time.time()}
        prev = (cur, t1)
        time.sleep(0.25)
    return {'tag': tag, 'mode': 'coarse', 'board_epoch': prev[0] if prev else None,
            'offset_s': round((prev[0] - prev[1]), 3) if prev else None,
            'wall': time.time()}


def log(ev):
    with (OUT / 'events.jsonl').open('a') as f:
        f.write(json.dumps(ev, ensure_ascii=False) + '\n')
    print(json.dumps(ev, ensure_ascii=False), flush=True)


# 1) 对时：把板子设成主机的 UTC 时间
now = time.gmtime()
stamp = time.strftime('%b %d %H:%M:%S %Y', now)
text, t0, t1 = run(f'date -u -s "{stamp}"; date -u +%s')
log({'tag': 'set', 'cmd': stamp, 'reply': text.strip()[-80:],
     'host_send': t0, 'host_recv': t1})

log(sample('t0'))

# 2) 静置采样
start = time.monotonic()
done = set()
while time.monotonic() - start < TOTAL:
    time.sleep(max(0.0, STEP - ((time.monotonic() - start) % STEP)))
    el = time.monotonic() - start
    tag = f'{el / 3600:.1f}h'
    for cp in CHECKPOINTS:
        if cp not in done and el >= cp:
            tag = f'CHECK-{cp // 3600}h'
            done.add(cp)
    try:
        log(sample(tag))
    except Exception as exc:                 # 网络抖动不终止长测
        log({'tag': tag, 'error': str(exc), 'wall': time.time()})

log({'tag': 'end', 'elapsed_s': round(time.monotonic() - start, 1)})
