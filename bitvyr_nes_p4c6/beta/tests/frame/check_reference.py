"""Проверяет полный regression stream и немодальный bad-key отказ actual Frame executable."""
from pathlib import Path
import hashlib
import subprocess
import sys

def main():
    """Не читает старые build artifacts: SHA задан независимым accepted full-output golden."""
    executable=Path(sys.argv[1]);output=Path(sys.argv[2]);output.parent.mkdir(parents=True,exist_ok=True)
    positive=subprocess.run([str(executable),str(output),'positive'],capture_output=True,timeout=40)
    if positive.returncode:raise SystemExit(positive.stderr.decode('utf-8','replace'))
    data=output.read_bytes()
    if len(data)!=2951808 or hashlib.sha256(data).hexdigest()!='66bce9181c494837a69aa7b5757ab34ac6a004810c14cfd3de069e413502b94a':
        raise SystemExit('CPU/RAM/fullRGB stream mismatch')
    negative=subprocess.run([str(executable),str(output.with_name('negative.bin')),'negative'],capture_output=True,timeout=20)
    if negative.returncode!=73 or b'negative_bad_key_rejected'not in negative.stderr:raise SystemExit('Bad-key negative did not reject')
    print('PASS actual Frame4scenarios/24frames CPU/RAM/fullRGB2951808B exact, fullkeys/earlyrelease/reuse, negative73')

if __name__=='__main__':main()
