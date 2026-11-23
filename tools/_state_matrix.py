import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port.exe')

fns = ['37F948', '37F910', '376F98', '37F9BC', '376F70', '37FBC8',
       '298730', '29880C']
for fn in fns:
    log = os.path.join(ROOT, 'out', 'logs', 'st_%s.txt' % fn)
    shot = os.path.join(ROOT, 'out', 'shot_st_%s.bmp' % fn)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '60'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_ENGINE_MENU'] = '1'
    env['HLE_FRAME_LOOP2'] = fn
    env['HLE_FRAME_ARG2'] = '3C6FE4,0,0,0'
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        subprocess.run([EXE, 'out\\romfs', '--shot', shot], cwd=ROOT,
                       env=env, stdout=fh, stderr=subprocess.STDOUT,
                       timeout=180)
    data = open(log, 'rb').read()
    s = data.decode('utf-16-le', errors='replace') if data[:2] == b'\xff\xfe' else data.decode('utf-8', errors='replace')
    files = len([l for l in s.splitlines() if '[fs] OpenFile' in l])
    dl = re.findall(r'\[port\] frames=\d+ pc=\S+ units=(\d+) dl=(\d+)/', s)
    unk = s.count('unbekanntes call-Ziel')
    print(fn, 'files=', files, 'unk=', unk, 'dl=', dl[-1] if dl else None)
