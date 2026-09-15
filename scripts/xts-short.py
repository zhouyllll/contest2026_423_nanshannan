#!/usr/bin/env python3
"""Run one bounded NSH command and preserve its raw serial output."""

import datetime as dt
import json
import os
import re
import sys
import time

import serial

command = sys.argv[1]
timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
outdir = '/tmp/k7-xts-short'
os.makedirs(outdir, exist_ok=True)
stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
tag = re.sub(r'[^A-Za-z0-9]+', '-', command).strip('-')[:60]
path = os.path.join(outdir, f'{stamp}-{tag}.raw')
prompt = re.compile(rb'nsh>\s*(?:\x1b\[[0-9;]*[A-Za-z])?\s*$')

with serial.Serial('/dev/ttyUSB0', 1500000, timeout=0.01,
                   exclusive=True) as port, open(path, 'wb') as rawfile:
    port.write(b'\r')
    ready = bytearray()
    end = time.monotonic() + 8
    while time.monotonic() < end:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            rawfile.write(chunk)
            ready.extend(chunk)
            if prompt.search(ready[-400:]):
                break
    if not prompt.search(ready[-400:]):
        raise RuntimeError('No NSH prompt')
    port.reset_input_buffer()
    for char in command.encode():
        port.write(bytes((char,)))
        time.sleep(0.003)
    start = time.time()
    start_mono = time.monotonic()
    port.write(b'\r')
    data = bytearray()
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            rawfile.write(chunk)
            data.extend(chunk)
            if prompt.search(data[-400:]):
                break
    done = time.time()
    record = {'command': command, 'host_at_enter': start,
              'host_at_done': done, 'elapsed_monotonic_s': time.monotonic() - start_mono,
              'host_clock_moved_back': done < start, 'raw': path,
              'prompt_returned': bool(prompt.search(data[-400:]))}
    with open(os.path.join(outdir, 'events.jsonl'), 'a', encoding='utf-8') as events:
        events.write(json.dumps(record, ensure_ascii=False) + '\n')
    print(data.decode('utf-8', 'replace'))
    print(json.dumps(record, ensure_ascii=False))
    if not record['prompt_returned']:
        sys.exit(2)
