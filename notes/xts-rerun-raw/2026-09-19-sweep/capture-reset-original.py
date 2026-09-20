import json,time,re
from pathlib import Path
import serial
out=Path('/tmp/k7-xts-reset-0919');out.mkdir(exist_ok=True)
with serial.Serial('/dev/ttyUSB0',1500000,timeout=.05,write_timeout=2,exclusive=True) as s:
    def read_for(t):
        end=time.monotonic()+t; b=bytearray()
        while time.monotonic()<end: b.extend(s.read(8192))
        return bytes(b)
    s.write(b'\r'); initial=read_for(2);(out/'initial.raw').write_bytes(initial)
    if b'nsh>' not in initial: raise SystemExit('No initial NSH')
    commands=[(f'watchdog-{i}',f'cmocka_driver_watchdog -r {i}',25) for i in range(4)]
    commands += [(f'reboot-{i:02}','reboot',20) for i in range(1,11)]
    for tag,cmd,limit in commands:
        for b in cmd.encode(): s.write(bytes([b]));time.sleep(.005)
        start=time.monotonic();s.write(b'\r'); data=bytearray();first_nsh=None;chunks=[]
        while time.monotonic()-start<limit:
            b=s.read(8192)
            if b:
                elapsed=time.monotonic()-start;chunks.append({'offset':len(data),'size':len(b),'monotonic_s':elapsed});data.extend(b)
                if first_nsh is None and b'NuttShell (NSH)' in data: first_nsh=elapsed
        (out/f'{tag}.raw').write_bytes(data)
        (out/f'{tag}-chunks.json').write_text(json.dumps(chunks))
        s.write(b'\r');probe=read_for(2);(out/f'{tag}-probe.raw').write_bytes(probe)
        event=dict(command=cmd,tag=tag,elapsed_monotonic_s=time.monotonic()-start,first_nsh_monotonic_s=first_nsh,prompt_after=b'nsh>' in probe,fail_marker=b'FAILED' in data,boot_banner=b'NuttShell (NSH)' in data)
        with (out/'events.jsonl').open('a') as f:f.write(json.dumps(event)+'\n')
        print(json.dumps(event),flush=True)
        if not event['prompt_after']:break
