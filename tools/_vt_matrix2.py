import os
import re
import subprocess

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port_scan.exe')

fns = ['37F948', '37F910', '376F98', '37F9BC', '376F70', '37FBC8',
       '298730', '29880C', '29BB78', '29886C', '244A48', '2449A4']
for fn in fns:
    log = os.path.join(ROOT, 'out', 'logs', 'vt2_%s.txt' % fn)
    shot = os.path.join(ROOT, 'out', 'shot_vt2_%s.bmp' % fn)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '120'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_ENGINE_MENU'] = '1'
    env['HLE_MENU_DUMP'] = '1'
    env['HLE_MENU_PARENT'] = '1'
    env['HLE_FRAME_LOOP2'] = fn
    env['HLE_FRAME_ARG2'] = '3EE12C,0,0,0'
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        subprocess.run([EXE, 'out\\romfs', '--shot', shot], cwd=ROOT,
                       env=env, stdout=fh, stderr=subprocess.STDOUT,
                       timeout=240)
    data = open(log, 'rb').read()
    s = data.decode('utf-16-le', errors='replace') if data[:2] == b'\xff\xfe' else data.decode('utf-8', errors='replace')
    opens = [l.split('"')[1] for l in s.splitlines() if '[fs] OpenFile' in l]
    other = sorted(set(x for x in opens if 'gamecoin' not in x))
    frames = re.findall(r'\[port\] frames=\d+ pc=\S+ units=(\d+) dl=(\d+)/(\d+)', s)
    last = frames[-1] if frames else ('?', '?', '?')
    lrs = re.findall(r'SetCommandList[^\n]*lr=([0-9A-F]+)', s)
    lr_engine = sum(1 for x in lrs if x != '001ED9C8')
    mobj = [l for l in s.splitlines() if l.startswith('[menuobj] raw')]
    dlw = ''
    if mobj:
        w = mobj[0].split(':')[1].split()
        dlw = 'nodeptr=%s nodecnt=%s' % (w[27], w[29])
    print('%s units=%s dl=%s/%s englist=%d/%d other=%s %s' %
          (fn, last[0], last[1], last[2], lr_engine, len(lrs),
           ','.join(other[:3]) if other else '-', dlw))
