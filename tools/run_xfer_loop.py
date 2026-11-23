import os
import subprocess
import sys

ROOT = r'C:\Users\user\Documents\Repos\3DS'
log = os.path.join(ROOT, 'out', 'logs', 'initworld_run.txt')
for i in range(6):
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'run_initworld.py')],
                   cwd=ROOT, capture_output=True)
    txt = open(log, encoding='utf-8', errors='replace').read()
    if '[xfer]' in txt:
        print('run', i + 1, 'xfer')
        break
for line in txt.splitlines():
    if ('drain-in' in line or '[xfer]' in line or 'ring2' in line
            or 'rendinit' in line):
        print(line)
