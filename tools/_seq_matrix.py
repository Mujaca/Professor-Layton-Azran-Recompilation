import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port.exe')

fns = ['10470C', '1047DC', '1047E4', '104420', '104428', '104440']
for fn in fns:
    log = os.path.join(ROOT, 'out', 'logs', 'seq_%s.txt' % fn)
    shot = os.path.join(ROOT, 'out', 'shot_seq_%s.bmp' % fn)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '60'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_ENGINE_MENU'] = '1'
    env['HLE_FRAME_LOOP2'] = fn
    env['HLE_FRAME_ARG2'] = '3EE12C,0,0,0'
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        subprocess.run([EXE, 'out\\romfs', '--shot', shot], cwd=ROOT,
                       env=env, stdout=fh, stderr=subprocess.STDOUT,
                       timeout=180)
