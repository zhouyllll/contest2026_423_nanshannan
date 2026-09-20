import importlib.util, os, sys, time, json, threading
from pathlib import Path
import serial

phase2 = len(sys.argv)>1 and sys.argv[1]=='phase2'
out=Path('/tmp/k7-xts-sweep-0919-phase2' if phase2 else '/tmp/k7-xts-sweep-0919'); out.mkdir(exist_ok=True)
os.environ['K7_NETSH_OUT']=str(out)
spec=importlib.util.spec_from_file_location('netsh','/home/dministrator/openvela-amlogic/src/contest2026_423_nanshannan/scripts/xts-netsh.py')
netsh=importlib.util.module_from_spec(spec); spec.loader.exec_module(netsh)
stop=threading.Event()
def capture(port):
    with (out/'serial.raw').open('ab') as f:
        while not stop.is_set():
            b=port.read(4096)
            if b: f.write(b); f.flush()

commands=[('uname -a',10),('uptime',10),('free',10),('ls /dev',10),('mount',10),('df -h',10),('resetcause',10),
('ls -l /tmp/xts-uart-pattern.bin',10),('md5_test -f /tmp/xts-uart-pattern.bin -c 1',10),
('cmocka_driver_rtc',10),('nist_sts 400000',10),('cmocka_driver_oneshot -d /dev/oneshot',40),
('cmocka_des3cbc',30),('cmocka_aescbc',30),('cmocka_aesctr',30),('cmocka_aesxts',30),('cmocka_hmac',30),('cmocka_hash',30),('cmocka_crc32',30),('cmocka_ecdsa',40),
('cmocka_mm_test',40),('cmocka_sched_test',60),('cmocka_syscall_test',60),('getprime',15),('mm',60),('scanftest',30),('hello',10),('helloxx',10),('popen',30),('pipe',60),('cxxtest',30),('free',10),('ps',10)]
if phase2:
    commands=[('uname -a',10),('mount',10),('fstest -n 10 -m /tmp',120),
    ('mkrd -m 10 -s 1000 1024',10),('cmocka_driver_block -m /dev/ram10',120),
    ('mount -t tmpfs /etc',10),('echo xts-md5 > /etc/1.txt',10),('cat /etc/1.txt',10),
    ('md5_test -f /etc/1.txt -c 100',30),('free',10),
    ('dd if=/dev/mmcsd1 of=/dev/null bs=512 skip=4194304 count=1',15),
    ('show_info',10),('ps',10),('ostest',60)]
with serial.Serial('/dev/ttyUSB0',1500000,timeout=.1,exclusive=True) as port:
    thread=threading.Thread(target=capture,args=(port,)); thread.start()
    try:
        with (out/'console.txt').open('a') as console:
            original=sys.stdout
            for cmd,timeout in commands:
                mark={'command':cmd,'monotonic':time.monotonic(),'serial_offset':(out/'serial.raw').stat().st_size}
                with (out/'serial-markers.jsonl').open('a') as f: f.write(json.dumps(mark)+'\n')
                print('RUN '+cmd,flush=True)
                sys.argv=['xts-netsh.py',cmd,str(timeout)]
                try:
                    sys.stdout=console
                    rc=netsh.main()
                except Exception as e:
                    print(repr(e)); rc=3
                finally: sys.stdout=original; console.flush()
                print('DONE '+cmd+' rc='+str(rc),flush=True)
                time.sleep(.5)
                if rc: break
    finally: stop.set(); thread.join()
