import datetime
import os
from pathlib import Path
import subprocess
import time

repo = Path('/home/dministrator/openvela-amlogic/src/contest2026_423_nanshannan')
out = Path('/tmp/k7-xts-135-run')
env = dict(os.environ, K7_NETSH_OUT=str(out))
deadline = time.monotonic() + 86400
with (out / 'monitor.log').open('ab', buffering=0) as log:
    while time.monotonic() < deadline:
        log.write((datetime.datetime.now(datetime.timezone.utc).isoformat() + '\n').encode())
        try:
            result = subprocess.run(
                ['python3', 'scripts/xts-netsh.py', 'dmesg -c', '15'],
                cwd=repo, env=env, stdout=log, stderr=subprocess.STDOUT,
                timeout=25)
        except subprocess.TimeoutExpired:
            log.write(b'LOG_CAPTURE_TIMEOUT; board test is not cancelled\n')
        capture = (out / 'test-capture.log').read_text(errors='replace')
        if 'prompt_returned' in capture or 'Traceback' in capture:
            log.write(b'TEST_CAPTURE_ENDED; inspect result and last syslog before assigning PASS\n')
            break
        time.sleep(30)
    else:
        log.write(b'MONITOR_DEADLINE; no PASS inferred; inspect board task state\n')
