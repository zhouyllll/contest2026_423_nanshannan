"""长时间抓串口，边抓边落盘（不等结束才写，随时可查）。"""
import sys, time
from pathlib import Path
import serial
dur=float(sys.argv[1]); out=Path(sys.argv[2])
s=serial.Serial('/dev/ttyUSB0',1500000,timeout=0.2,exclusive=True)
f=open(out,'ab'); t0=time.time()
while time.time()-t0<dur:
    b=s.read(65536)
    if b: f.write(b); f.flush()
