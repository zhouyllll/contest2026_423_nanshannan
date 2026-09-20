"""xTS 1.2.2 / 2.1.3：等人手按复位或上下电，逐次记录冷启动日志与时间。

用法：python3 capture.py <次数> <输出目录> <标签: reset|power>

判据：
- t0 = 本次启动串口收到的**第一个字节**（对应原文 minicom 时间戳的起点；
  实测第一行 DDR 日志在上电后几十毫秒内）。
- 启动完成 = 字节流里凑齐 "NuttShell (NSH)"（只在 openvela 日志段内、
  跨度 ≤400 字节的子序列；本板 syslog 与 NSH 横幅逐字节交错，连续匹配找不到）。
- 每次保存原始字节，并列出 U-Boot 之后含 error/fail/assert/panic/timeout 的行。

新的一次启动：串口静默 ≥1.5s 之后再次出现字节。
"""
import json, re, sys, time
from pathlib import Path
import serial

N = int(sys.argv[1]); OUT = Path(sys.argv[2]); TAG = sys.argv[3]
OUT.mkdir(parents=True, exist_ok=True)
B = b'NuttShell (NSH)'
ERR = re.compile(rb'error|fail|assert|panic|timeout|timed out', re.I)
DEADLINE = 40 * 60


def banner_time(data, chunks, start):
    for i in range(start, len(data)):
        if data[i] != B[0]:
            continue
        k = 0; j = i
        while j < len(data) and j - i <= 400 and k < len(B):
            if data[j] == B[k]:
                k += 1
            j += 1
        if k == len(B):
            off = j - 1
            return next(c['t'] for c in chunks if c['off'] + c['n'] > off)
    return None


s = serial.Serial('/dev/ttyUSB0', 1500000, timeout=0.05, exclusive=True)
print(f'准备好了：请开始第 1 次（{TAG}），共 {N} 次。每次等日志停下再做下一次。', flush=True)
s.read(1 << 20)
t_start = time.monotonic()
done = 0
while done < N and time.monotonic() - t_start < DEADLINE:
    b = s.read(65536)
    if not b:
        continue
    t0 = time.monotonic()
    data = bytearray(b); chunks = [{'off': 0, 'n': len(b), 't': 0.0}]
    last = t0
    while time.monotonic() - last < 6.0:
        b = s.read(65536)
        if b:
            chunks.append({'off': len(data), 'n': len(b),
                           't': round(time.monotonic() - t0, 4)})
            data += b; last = time.monotonic()
    done += 1
    tag = f'{TAG}-{done:02}'
    (OUT / f'{tag}.raw').write_bytes(data)
    (OUT / f'{tag}-chunks.json').write_text(json.dumps(chunks))
    u = data.find(b'U-Boot'); c = data.find(b'[CPU0]', u if u >= 0 else 0)
    bt = banner_time(data, chunks, c if c >= 0 else 0)
    errs = [l.decode('utf-8', 'replace').strip()[:160]
            for l in bytes(data[u:]).splitlines() if u >= 0 and ERR.search(l)]
    ev = {'tag': tag, 'bytes': len(data), 'banner_s': bt, 'errors': errs}
    with (OUT / 'events.jsonl').open('a') as f:
        f.write(json.dumps(ev, ensure_ascii=False) + '\n')
    print(f'第 {done}/{N} 次：{len(data)} 字节，'
          f'NuttShell (NSH) {"%.3f s" % bt if bt else "未找到"}，'
          f'异常行 {len(errs)}', flush=True)
    if done < N:
        print(f'  → 请做第 {done + 1} 次', flush=True)
print('采集结束', flush=True)
