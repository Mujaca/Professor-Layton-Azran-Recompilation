import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port.exe')

cases = [('28E40C', '3EC028'), ('11AF00', '3EC028'),
         ('11B02C', '3EC028'), ('121F3C', '3EC5C4')]
for fn, arg in cases:
    log = os.path.join(ROOT, 'out', 'logs', 'sub_%s.txt' % fn)
    shot = os.path.join(ROOT, 'out', 'shot_sub_%s.bmp' % fn)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '60'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_ENGINE_MENU'] = '1'
    env['HLE_FRAME_LOOP2'] = fn
    env['HLE_FRAME_ARG2'] = arg + ',0,0,0'
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        subprocess.run([EXE, 'out\\romfs', '--shot', shot], cwd=ROOT,
                       env=env, stdout=fh, stderr=subprocess.STDOUT,
                       timeout=180)
    data = open(log, 'rb').read()
    s = data.decode('utf-16-le', errors='replace') if data[:2] == b'\xff\xfe' else data.decode('utf-8', errors='replace')
    files = [l for l in s.splitlines() if '[fs] OpenFile' in l]
    dl = re.findall(r'\[port\] frames=\d+ pc=\S+ units=(\d+) dl=(\d+)/', s)
    unk = s.count('unbekanntes call-Ziel')
    print(fn, arg, 'files=', len(files), 'unk=', unk,
          'dl_last=', dl[-1] if dl else None)
