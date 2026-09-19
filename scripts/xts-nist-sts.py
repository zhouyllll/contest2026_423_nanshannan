#!/usr/bin/env python3
"""xTS 1.3.16：经网络 NSH 按原文步骤跑 nist_sts，并取回 finalAnalysisReport.txt。

用法：scripts/xts-nist-sts.py <输出目录> [输入文件，默认 /dev/urandom/]

步骤照 docs/refs/openvela_xts_test_cases.md 1.3.16：
  cd /tmp；mkdir -p experiments/AlgorithmTesting/<15 个子目录>；
  nist_sts 400000；依次输入 0、/dev/urandom/、1、0、10、1；
  cat /tmp/experiments/AlgorithmTesting/finalAnalysisReport.txt

原文没写、但 NonOverlappingTemplate 必需的一步：程序按相对路径读
templates/template9（m=9）。板上没有，这里逐行 echo 写到 /tmp/templates/，
写完 cat 回来与 NIST 原始文件逐字比对，不一致就不跑。

全程原始字节存 <输出目录>/session.raw，文本存 session.txt。
"""

import importlib.util
import os
import socket
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('netsh', HERE / 'xts-netsh.py')
netsh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(netsh)

TEMPLATE = (HERE / '../../apps/testing/drivers/nist-sts/nist-sts/sts/templates/template9').resolve()
SUBDIRS = ['ApproximateEntropy', 'CumulativeSums', 'Frequency', 'LongestRun',
           'OverlappingTemplate', 'RandomExcursionsVariant', 'Runs',
           'Universal', 'BlockFrequency', 'FFT', 'LinearComplexity',
           'NonOverlappingTemplate', 'RandomExcursions', 'Rank', 'Serial']


class Session:
    def __init__(self, out):
        self.conn = socket.create_connection((netsh.HOST, netsh.PORT), timeout=5)
        self.conn.settimeout(0.1)
        self.filt = netsh.TelnetFilter()
        self.raw = open(out / 'session.raw', 'wb')
        self.text = bytearray()
        greet = netsh.receive(self.conn, self.filt, self.raw, 10)
        self.text += greet
        if not netsh.PROMPT.search(greet[-400:]):
            raise RuntimeError('No network NSH prompt')

    def cmd(self, line, timeout=30):
        self.conn.sendall(line.encode() + b'\r\n')
        out = netsh.receive(self.conn, self.filt, self.raw, timeout)
        self.text += out
        if not netsh.PROMPT.search(out[-400:]):
            raise RuntimeError(f'no prompt after: {line}')
        return out

    def until(self, keys, timeout):
        """读到输出里出现 keys 中任意一个（只看新到的字节）。"""
        end = time.monotonic() + timeout
        buf = bytearray()
        while time.monotonic() < end:
            try:
                raw = self.conn.recv(65536)
            except socket.timeout:
                continue
            if not raw:
                break
            self.raw.write(raw)
            self.raw.flush()
            plain = self.filt.feed(self.conn, raw)
            buf += plain
            self.text += plain
            for k in keys:
                if k in buf:
                    return k, bytes(buf)
        raise RuntimeError(f'timeout waiting for {keys}: {bytes(buf[-300:])!r}')

    def quiet(self, seconds, timeout=120):
        """等到连续 seconds 秒没有新输出。"""
        end = time.monotonic() + timeout
        last = time.monotonic()
        while time.monotonic() < end:
            try:
                raw = self.conn.recv(65536)
            except socket.timeout:
                if time.monotonic() - last >= seconds:
                    return
                continue
            if not raw:
                raise RuntimeError('connection closed')
            self.raw.write(raw)
            self.raw.flush()
            self.text += self.filt.feed(self.conn, raw)
            last = time.monotonic()
        raise RuntimeError('output never went quiet')

    def answer(self, value):
        """★ 不能等提示文字：nist_sts 的提示（"Enter Choice: " 等）不带换行，
        NuttX 的 stdout 不会因为读 stdin 而刷新，提示会一直留在板上的
        缓冲里 —— 等它就是互相等死（第一次就这样卡住了）。改为输出安静
        1.5 秒、程序必然已停在 scanf 上时再送答案。"""
        self.quiet(1.5)
        self.conn.sendall(value.encode() + b'\r\n')


def main():
    out = Path(sys.argv[1])
    source = sys.argv[2] if len(sys.argv) > 2 else '/dev/urandom/'
    out.mkdir(parents=True, exist_ok=True)
    s = Session(out)
    try:
        s.cmd('cd /tmp')
        for d in SUBDIRS:
            s.cmd(f'mkdir -p experiments/AlgorithmTesting/{d}')

        # 模板：逐行写入并比对
        lines = TEMPLATE.read_text().splitlines()
        s.cmd('mkdir -p templates')
        s.cmd('rm -f templates/template9')
        for line in lines:
            s.cmd(f'echo "{line}" >> templates/template9')
        back = s.cmd('cat templates/template9', 60).decode('utf-8', 'replace')
        got = [l.strip() for l in back.replace('\r', '').split('\n')
               if l.strip() and set(l.strip()) <= set('01 ')]
        want = [l.strip() for l in lines]
        if got != want:
            raise RuntimeError(f'template9 读回不一致：{len(got)} / {len(want)} 行')
        print(f'template9 已写入并比对一致（{len(want)} 行）', flush=True)

        # 原文步骤
        t0 = time.monotonic()
        s.conn.sendall(b'nist_sts 400000\r\n')
        s.until([b'G Using SHA-1'], 60)       # 发生器菜单最后一行
        for value in ('0', source, '1', '0', '10', '1'):
            s.answer(value)
        key, _ = s.until([b'nsh>'], 4 * 3600)
        print(f'nist_sts 结束，用时 {time.monotonic() - t0:.0f}s', flush=True)
        time.sleep(0.5)
        report = s.cmd('cat /tmp/experiments/AlgorithmTesting/finalAnalysisReport.txt', 120)
        (out / 'finalAnalysisReport.txt').write_bytes(report)
        print(report.decode('utf-8', 'replace'))
    finally:
        (out / 'session.txt').write_bytes(bytes(s.text))
        s.raw.close()
        s.conn.close()


if __name__ == '__main__':
    main()
