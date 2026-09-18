import json
import time
from pathlib import Path
import serial

out = Path('/tmp/k7-xts-1310')
out.mkdir(exist_ok=True)
expected = b"0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ,./<>?;':\"[]{}\\|!@#$%^&*()-+_="
with serial.Serial('/dev/ttyUSB0', 1500000, timeout=.05, write_timeout=1, exclusive=True) as port:
    def receive(seconds):
        data = bytearray()
        until = time.monotonic() + seconds
        while time.monotonic() < until:
            data.extend(port.read(4096))
        return bytes(data)

    def send(data):
        for byte in data:
            port.write(bytes([byte]))
            time.sleep(.005)

    port.write(b'\r')
    initial = receive(2)
    (out / 'initial.raw').write_bytes(initial)
    if b'nsh>' not in initial:
        raise SystemExit('No serial NSH; no test sent')

    for case in (0, 1, 2):
        cmd = f'cmocka_driver_uart -d /dev/ttyS0 -n {case}'
        start = time.monotonic()
        send(cmd.encode() + b'\r')
        data = receive(1)
        sent = bytearray()
        matched = None
        if case == 1 and b'nsh>' not in data:
            send(expected)
            sent.extend(expected)
            data += receive(3)
        elif case == 2 and b'nsh>' not in data:
            payload = b'UART-xts-0123456789'
            frame = str(len(payload)).encode() + b'#' + payload
            send(frame)
            sent.extend(frame)
            echo = receive(2)
            (out / 'burst-echo.raw').write_bytes(echo)
            matched = payload in echo
            reply = b'pass#0#' if matched else b'fail#'
            send(reply)
            sent.extend(reply)
            data += echo + receive(3)
        (out / f'case-{case}.raw').write_bytes(data)
        (out / f'case-{case}-sent.bin').write_bytes(sent)
        record = dict(command=cmd, elapsed_monotonic_s=time.monotonic()-start,
                      prompt_returned=b'nsh>' in data,
                      expected_send_seen=expected in data if case == 0 else None,
                      burst_payload_seen=matched)
        with (out / 'events.jsonl').open('a') as f:
            f.write(json.dumps(record)+'\n')
        print(json.dumps(record), flush=True)
        print(data.decode(errors='replace'), flush=True)
        if b'nsh>' not in data:
            break
