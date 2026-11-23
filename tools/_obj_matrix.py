import os
import subprocess

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'out', 'port.exe')
TOP = open(os.path.join(ROOT, 'out', 'menu_top.bin'), 'rb').read()
BOT = open(os.path.join(ROOT, 'out', 'menu_bottom.bin'), 'rb').read()


def diff(a, b):
    n = 0
    for i in range(0, min(len(a), len(b)), 3):
        if abs(a[i] - b[i]) + abs(a[i + 1] - b[i + 1]) + abs(a[i + 2] - b[i + 2]) > 60:
            n += 1
    return n


def reg(d, w, x0, y0, rw, rh):
    o = bytearray()
    for y in range(rh):
        o += d[((y0 + y) * w + x0) * 3:((y0 + y) * w + x0 + rw) * 3]
    return bytes(o)


objs = ['3CA494', '3EE12C', '3C6FE4', '3EC028', '3EE188', '3CA4A0']
for obj in objs:
    shot = os.path.join(ROOT, 'out', 'shot_obj_%s.bmp' % obj)
    log = os.path.join(ROOT, 'out', 'logs', 'obj_%s.txt' % obj)
    env = dict(os.environ)
    env['PORT_SHOT_FRAMES'] = '60'
    env['PORT_NO_OFFICIAL'] = '1'
    env['HLE_REAL_ENTRY'] = '1'
    env['HLE_ENTRY_FN'] = '36C6E8'
    env['HLE_ENTRY_OBJ'] = obj
    env['HLE_LOG'] = '1'
    with open(log, 'w', encoding='utf-8', errors='replace') as fh:
        p = subprocess.run([EXE, 'out\\romfs', '--shot', shot],
                           cwd=ROOT, env=env, stdout=fh,
                           stderr=subprocess.STDOUT, timeout=180)
    txt = open(log, encoding='utf-8', errors='replace').read()
    sets = txt.count('[gsp] SetCommandList')
    lrs = set()
    for line in txt.splitlines():
        if '[gsp] SetCommandList' in line and 'lr=' in line:
            lrs.add(line.split('lr=')[-1])
    unk = txt.count('unbekanntes call-Ziel')
    im = Image.open(shot).convert('RGB')
    d = im.tobytes()
    w, h = im.size
    t = diff(reg(d, w, 0, 0, 400, 240), TOP)
    b = diff(reg(d, w, 40, 240, 320, 240), BOT)
    print('%s exit=%d sets=%d unk=%d TOP=%d BOT=%d lr=%s' %
          (obj, p.returncode, sets, unk, t, b, sorted(lrs)))
