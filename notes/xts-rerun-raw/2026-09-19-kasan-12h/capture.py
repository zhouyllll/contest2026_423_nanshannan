"""xTS 3.1.1：独占串口抓 N 秒原始字节，按块记录到达时刻；结束时统计异常关键词。

用法：python3 capture.py <秒数> <输出目录>
- serial.raw：原始字节；chunks.jsonl：每块 {offset,size,t(自开始的秒), wall}
- summary.json：U-Boot 横幅出现次数（=重启次数）、panic/assert/kasan/error 等关键词行
  每 10 分钟刷新一次 summary，中途也能看。
"""
import json, re, sys, time
from pathlib import Path
import serial

DUR = float(sys.argv[1]); OUT = Path(sys.argv[2]); OUT.mkdir(parents=True, exist_ok=True)
KEYS = re.compile(rb'kasan|panic|assert|up_assert|crash|exception|ESR=|U-Boot 2017|'
                  rb'error|fail|watchdog|reset', re.I)

def summary(buf, done):
    lines = [l for l in bytes(buf).split(b'\n') if KEYS.search(l)]
    s = {'done': done, 'bytes': len(buf), 'elapsed_s': round(time.monotonic() - t0, 1),
         'uboot_banners': bytes(buf).count(b'U-Boot 2017'),
         'showinfo_lines': bytes(buf).count(b'Umem') + bytes(buf).count(b'mem'),
         'keyword_lines': [l.decode('utf-8', 'replace').strip()[:200] for l in lines][:500]}
    (OUT / 'summary.json').write_text(json.dumps(s, ensure_ascii=False, indent=1))

s = serial.Serial('/dev/ttyUSB0', 1500000, timeout=0.2, exclusive=True)
raw = open(OUT / 'serial.raw', 'ab'); ch = open(OUT / 'chunks.jsonl', 'a')
buf = bytearray(); t0 = time.monotonic(); last = t0
while time.monotonic() - t0 < DUR:
    b = s.read(65536)
    if b:
        ch.write(json.dumps({'offset': len(buf), 'size': len(b),
                             't': round(time.monotonic() - t0, 3), 'wall': time.time()}) + '\n')
        buf += b; raw.write(b); raw.flush(); ch.flush()
    if time.monotonic() - last > 600:
        summary(buf, False); last = time.monotonic()
summary(buf, True)
