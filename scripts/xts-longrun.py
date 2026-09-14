#!/usr/bin/env python3
"""K7 xTS 1.3.14 (24h UTC drift) + 3.1.1 (12h idle) serial recorder."""
import datetime as dt
import json
import os
import re
import sys
import time
import serial

PORT = os.environ.get('K7_SERIAL', '/dev/ttyUSB0')
BAUD = int(os.environ.get('K7_BAUD', '1500000'))
OUT = os.environ.get('K7_XTS_OUT', '/tmp/k7-xts-longrun')
PROMPT = re.compile(rb'nsh>\s*(?:\x1b\[[0-9;]*[A-Za-z])?\s*$')

os.makedirs(OUT, exist_ok=True)
serial_log = open(os.path.join(OUT, 'serial.log'), 'ab', buffering=0)
events = open(os.path.join(OUT, 'events.jsonl'), 'a', buffering=1)
s = serial.Serial(PORT, BAUD, timeout=0.2, exclusive=True)

def event(kind, **data):
    row = {'kind': kind, 'host_epoch': time.time(), 'host_utc': dt.datetime.now(dt.timezone.utc).isoformat(), **data}
    events.write(json.dumps(row, ensure_ascii=False) + '\n')
    print(json.dumps(row, ensure_ascii=False), flush=True)

def receive(seconds, stop_at_prompt=False):
    end = time.monotonic() + seconds
    buf = bytearray()
    while time.monotonic() < end:
        chunk = s.read(4096)
        if chunk:
            serial_log.write(chunk)
            buf.extend(chunk)
            if stop_at_prompt and PROMPT.search(buf[-400:]):
                return bytes(buf)
    return bytes(buf)

def command(cmd, timeout=8):
    s.reset_input_buffer()
    for c in cmd.encode():
        s.write(bytes((c,)))
        time.sleep(0.01)
    host_at_enter = time.time()
    s.write(b'\r')
    raw = receive(timeout, True)
    if not PROMPT.search(raw[-400:]):
        s.write(b'\r')
        raw += receive(3, True)
    event('command', command=cmd, host_at_enter=host_at_enter, output=raw.decode('utf-8', 'replace')[-1000:])
    if not PROMPT.search(raw[-400:]):
        raise RuntimeError('No NSH prompt after command')
    return raw, host_at_enter

def board_epoch():
    raw, sent = command('date -u +%s')
    nums = re.findall(rb'(?m)^([0-9]{10})\r?$', raw)
    if not nums:
        raise RuntimeError('Board epoch missing')
    return int(nums[-1]), sent

try:
    s.write(b'\r')
    if not PROMPT.search(receive(10, True)[-400:]):
        raise RuntimeError('NSH prompt unavailable')
    now = dt.datetime.now(dt.timezone.utc)
    stamp = now.strftime('%b %d %H:%M:%S %Y')
    command(f'date -u -s "{stamp}"')
    board, sent = board_epoch()
    drift = board - sent
    if abs(drift) > 3:
        raise RuntimeError(f'Initial time mismatch: {drift:.2f}s')
    start_wall = time.time()
    start_mono = time.monotonic()
    event('start', board_epoch=board, host_at_probe=sent, initial_offset_s=drift,
          standby_due_epoch=start_wall + 12*3600,
          drift_due_epoch=start_wall + 24*3600)
    for hour in (6, 12, 18, 24):
        due = start_mono + hour*3600
        while time.monotonic() < due:
            receive(min(30, due-time.monotonic()))
        board, sent = board_epoch()
        event('checkpoint', hour=hour, board_epoch=board, host_at_probe=sent,
              offset_s=board-sent)
        if hour == 12:
            event('standby_12h_complete', note='Inspect serial.log for crash, reboot or errors before marking PASS')
    event('drift_24h_complete')
except Exception as exc:
    event('error', message=str(exc))
    sys.exit(1)
finally:
    s.close()
    events.close()
    serial_log.close()
