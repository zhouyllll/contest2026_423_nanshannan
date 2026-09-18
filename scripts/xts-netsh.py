#!/usr/bin/env python3
"""Capture one xTS command through K7's TCP Telnet NSH test endpoint."""

import datetime as dt
import json
import os
import re
import socket
import sys
import time

HOST = os.environ.get('K7_NETSH_HOST', '192.168.1.50')
PORT = int(os.environ.get('K7_NETSH_PORT', '2323'))
OUT = os.environ.get('K7_NETSH_OUT', '/tmp/k7-xts-netsh')
PROMPT = re.compile(rb'nsh>\s*(?:\x1b\[[0-9;]*[A-Za-z])?\s*$')
IAC = 255
DO = 253
DONT = 254
WILL = 251
WONT = 252
SB = 250
SE = 240


class TelnetFilter:
    """Strip Telnet controls and decline options while preserving raw data."""

    def __init__(self):
        self.state = 'data'
        self.verb = 0

    def feed(self, connection, raw):
        plain = bytearray()
        for byte in raw:
            if self.state == 'data':
                if byte == IAC:
                    self.state = 'iac'
                else:
                    plain.append(byte)
            elif self.state == 'iac':
                if byte == IAC:
                    plain.append(IAC)
                    self.state = 'data'
                elif byte in (DO, DONT, WILL, WONT):
                    self.verb = byte
                    self.state = 'option'
                elif byte == SB:
                    self.state = 'subneg'
                else:
                    self.state = 'data'
            elif self.state == 'option':
                if self.verb == DO:
                    connection.sendall(bytes((IAC, WONT, byte)))
                elif self.verb == WILL:
                    connection.sendall(bytes((IAC, DONT, byte)))
                self.state = 'data'
            elif self.state == 'subneg':
                if byte == IAC:
                    self.state = 'subneg_iac'
            elif self.state == 'subneg_iac':
                self.state = 'data' if byte == SE else 'subneg'
        return bytes(plain)


def receive(connection, filtering, rawfile, seconds):
    end = time.monotonic() + seconds
    plain = bytearray()
    while time.monotonic() < end:
        try:
            raw = connection.recv(65536)
        except socket.timeout:
            continue
        if not raw:
            break
        rawfile.write(raw)
        rawfile.flush()
        plain.extend(filtering.feed(connection, raw))
        if PROMPT.search(plain[-400:]):
            break
    return bytes(plain)


def main():
    if len(sys.argv) < 2:
        print('usage: xts-netsh.py <NSH command> [timeout_seconds]', file=sys.stderr)
        return 2
    command = sys.argv[1]
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
    os.makedirs(OUT, exist_ok=True)
    stamp = dt.datetime.now(dt.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ') + '-' + str(time.monotonic_ns())
    tag = re.sub(r'[^A-Za-z0-9]+', '-', command).strip('-')[:60]
    rawpath = os.path.join(OUT, f'{stamp}-{tag}.raw')
    textpath = os.path.join(OUT, f'{stamp}-{tag}.txt')
    filtering = TelnetFilter()
    with socket.create_connection((HOST, PORT), timeout=5) as connection, \
            open(rawpath, 'wb') as rawfile:
        connection.settimeout(0.1)
        greeting = receive(connection, filtering, rawfile, 10)
        if not PROMPT.search(greeting[-400:]):
            raise RuntimeError('No network NSH prompt')
        start_wall = time.time()
        start_mono = time.monotonic()
        connection.sendall(command.encode() + b'\r\n')
        output = receive(connection, filtering, rawfile, timeout)
    with open(textpath, 'wb') as textfile:
        textfile.write(greeting + output)
    record = {'command': command, 'host': HOST, 'port': PORT,
              'host_at_enter': start_wall, 'host_at_done': time.time(),
              'elapsed_monotonic_s': time.monotonic() - start_mono,
              'host_clock_moved_back': time.time() < start_wall,
              'raw': rawpath, 'text': textpath,
              'prompt_returned': bool(PROMPT.search(output[-400:]))}
    with open(os.path.join(OUT, 'events.jsonl'), 'a', encoding='utf-8') as events:
        events.write(json.dumps(record, ensure_ascii=False) + '\n')
    print(output.decode('utf-8', 'replace'))
    print(json.dumps(record, ensure_ascii=False))
    return 0 if record['prompt_returned'] else 2


if __name__ == '__main__':
    sys.exit(main())
