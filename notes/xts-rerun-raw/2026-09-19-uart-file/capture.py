import os, pty, tty, select, subprocess, time, json, hashlib
from pathlib import Path
import serial

out = Path('/tmp/k7-xts-1311')
out.mkdir(exist_ok=True)
(out/'received').mkdir(exist_ok=True)
payload = out/'xts-uart-pattern.bin'
payload.write_bytes(bytes(range(256))*16)
events = []
with serial.Serial('/dev/ttyUSB0',1500000,timeout=.05,write_timeout=2,exclusive=True) as s:
    def read_for(seconds):
        result=bytearray(); end=time.monotonic()+seconds
        while time.monotonic()<end: result.extend(s.read(4096))
        return bytes(result)
    def command(cmd):
        for b in cmd.encode()+b'\r':
            s.write(bytes([b])); time.sleep(.005)
    s.write(b'\r'); initial=read_for(2)
    (out/'initial.raw').write_bytes(initial)
    if b'nsh>' not in initial: raise SystemExit('No NSH; no transfer started')
    for name, cmd, argv in [
        ('upload','rb -f /tmp --retry 5',['/usr/bin/sb','--ymodem','-b',str(payload)]),
        ('download','sb --retry 5 /tmp/xts-uart-pattern.bin',['/usr/bin/rb','--ymodem','-b','-y'])]:
        master,slave=pty.openpty(); tty.setraw(slave)
        command(cmd)
        start=time.monotonic()
        with (out/f'{name}.stderr').open('wb') as err, (out/f'{name}-rx.raw').open('wb') as rx, (out/f'{name}-tx.raw').open('wb') as tx:
            proc=subprocess.Popen(argv,stdin=slave,stdout=slave,stderr=err,cwd=out/'received')
            os.close(slave)
            try:
                while proc.poll() is None and time.monotonic()-start<40:
                    ready,_,_=select.select([s.fileno(),master],[],[],.1)
                    if s.fileno() in ready:
                        data=s.read(s.in_waiting or 1); rx.write(data); rx.flush(); os.write(master,data)
                    if master in ready:
                        try: data=os.read(master,8192)
                        except OSError: break
                        tx.write(data); tx.flush(); s.write(data)
            finally:
                if proc.poll() is None: proc.terminate()
                try: proc.wait(timeout=2)
                except subprocess.TimeoutExpired: proc.kill(); proc.wait()
                os.close(master)
        tail=read_for(2)
        if b'nsh>' not in tail:
            s.write(b'\x18'*8); tail+=read_for(3)
            s.write(b'\r'); tail+=read_for(2)
        (out/f'{name}-tail.raw').write_bytes(tail)
        event=dict(direction=name,command=cmd,host_argv=argv,returncode=proc.returncode,elapsed=time.monotonic()-start,prompt_returned=b'nsh>' in tail)
        events.append(event); print(json.dumps(event),flush=True)
        if not event['prompt_returned']: break
        if name=='upload' and proc.returncode!=0: break
result=out/'received'/payload.name
events.append(dict(source_sha256=hashlib.sha256(payload.read_bytes()).hexdigest(),received_sha256=hashlib.sha256(result.read_bytes()).hexdigest() if result.exists() else None,exact_match=result.exists() and result.read_bytes()==payload.read_bytes()))
(out/'events.json').write_text(json.dumps(events,indent=2)+'\n')
print(json.dumps(events[-1]),flush=True)
