"""xTS 1.2.1 Reboot 启动异常 / 2.1.4 Reboot 启动时间：串口 NSH 输入 reboot，共 N 次。

判据与限制（写进每条事件）：
- 起点 t0：在串口写出 "reboot" 后的回车那一刻（monotonic）。
- banner_s：字节流里按**子序列**首次凑齐 "NuttShell (NSH)" 的时刻。本板 syslog 用
  up_putc 直写 UART，NSH 横幅会与另一核的 syslog 逐字节交错（实测
  "NCu0t:t S2h5eMl l"），连续字符串匹配永远找不到；子序列匹配容忍被插字，
  但若横幅字节被主机 CH340 丢掉仍会找不到（记为 null）。
- prompt_s：辅助判据。看到 U-Boot 之后又出现 openvela 的 "[CPU0]" 才开始每 0.3s
  发回车（不在 U-Boot 1 秒倒计时里发，避免打断自动启动），记录首次收到
  "nsh>" 的时刻。它是"NSH 可交互"的上界，与横幅时刻不同。
- 每次抓满 CAPTURE_S 秒原始字节，列出 U-Boot 之后含 error/fail/assert/panic/
  timeout 的行，供 1.2.1 人工判读（不自动判 PASS）。
"""
import json, re, sys, time
from pathlib import Path
import serial

N = int(sys.argv[1]) if len(sys.argv) > 1 else 10
OUT = Path(sys.argv[2] if len(sys.argv) > 2 else '/tmp/k7-xts-reboot-run')
CAPTURE_S = 20.0
BANNER = b'NuttShell (NSH)'
ERR = re.compile(rb'error|fail|assert|panic|timeout|timed out', re.I)
OUT.mkdir(parents=True, exist_ok=True)


def read_for(s, t):
    end = time.monotonic() + t
    b = bytearray()
    while time.monotonic() < end:
        b.extend(s.read(8192))
    return bytes(b)


with serial.Serial('/dev/ttyUSB0', 1500000, timeout=0.02, exclusive=True) as s:
    for n in range(1, N + 1):
        tag = f'reboot-{n:02}'
        s.write(b'\r')
        pre = read_for(s, 1.0)
        if b'nsh>' not in pre:
            print(json.dumps({'tag': tag, 'abort': 'no nsh> before reboot'}))
            break
        for ch in b'reboot':
            s.write(bytes([ch]))
            time.sleep(0.005)
        read_for(s, 0.2)                       # 回显
        s.write(b'\r')
        t0 = time.monotonic()
        data = bytearray()
        chunks = []
        uboot_at = None
        os_at = None
        prompt_s = None
        banner_s = None
        bi = 0                                 # 子序列匹配进度
        last_kick = 0.0
        scan = 0
        while True:
            el = time.monotonic() - t0
            if el >= CAPTURE_S:
                break
            b = s.read(8192)
            if b:
                chunks.append({'offset': len(data), 'size': len(b), 'monotonic_s': round(el, 4)})
                data.extend(b)
                if uboot_at is None:
                    i = data.find(b'U-Boot')
                    if i >= 0:
                        uboot_at = i
                        scan = i
                if uboot_at is not None:
                    if os_at is None and data.find(b'[CPU0]', uboot_at) >= 0:
                        os_at = el
                    # 子序列：只在 U-Boot 之后扫
                    while scan < len(data) and banner_s is None:
                        if data[scan] == BANNER[bi]:
                            bi += 1
                            if bi == len(BANNER):
                                banner_s = el
                        scan += 1
                    if os_at is not None and prompt_s is None and \
                            data.find(b'nsh>', uboot_at) >= 0:
                        prompt_s = el
            if os_at is not None and prompt_s is None and el - last_kick >= 0.3:
                s.write(b'\r')
                last_kick = el
        (OUT / f'{tag}.raw').write_bytes(data)
        (OUT / f'{tag}-chunks.json').write_text(json.dumps(chunks))
        errs = []
        if uboot_at is not None:
            for line in bytes(data[uboot_at:]).splitlines():
                if ERR.search(line):
                    errs.append(line.decode('utf-8', 'replace').strip()[:160])
        ev = {'tag': tag, 'uboot_seen': uboot_at is not None,
              'openvela_log_s': os_at, 'banner_subseq_s': banner_s,
              'first_prompt_s': prompt_s, 'bytes': len(data),
              'error_lines': errs}
        with (OUT / 'events.jsonl').open('a') as f:
            f.write(json.dumps(ev, ensure_ascii=False) + '\n')
        print(json.dumps({k: ev[k] for k in ev if k != 'error_lines'}, ensure_ascii=False),
              '错误行', len(errs), flush=True)
