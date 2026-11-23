import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port.exe')

fns = ['2721E0', '2835EC', '286094', '286220', '289428', '28DE80', '28E02C']
for fn in fns:
    log = os.path.join(ROOT, 'out', 'logs', 'add_%s.txt' % fn)
    shot = os.path.join(ROOT, 'out', 'shot_add_%s.bmp' % fn)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '60'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_ENGINE_MENU'] = '1'
    env['HLE_FRAME_LOOP2'] = fn
    env['HLE_FRAME_ARG2'] = '3EE12C,0CFF2100,0,0'
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        p = subprocess.run([EXE, 'out\\romfs', '--shot', shot], cwd=ROOT,
                           env=env, stdout=fh, stderr=subprocess.STDOUT,
                           timeout=180)
    txt = open(log, encoding='utf-8', errors='replace').read()
    frames = re.findall(r'\[port\] frames=\d+ pc=\S+ units=(\d+) dl=(\d+)/(\d+)', txt)
    last = frames[-1] if frames else ('?', '?', '?')
    unk = txt.count('unbekanntes call-Ziel')
    fl = txt.count('[frameloop2]')
    print('%s fl=%d unk=%d units=%s dl=%s/%s' % (fn, fl, unk, last[0], last[1], last[2]))
