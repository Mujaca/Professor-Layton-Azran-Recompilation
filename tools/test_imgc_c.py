"""Regressionstest fuer runtime/gpu/imgc.c (IMGC: ETC1, 24/32 bpp, Kachelordnung).

  python tools/test_imgc_c.py

Dekodiert die Faelle aus tests/golden/imgc.json mit dem C-Dekoder und
vergleicht SHA-256 der RGB-Ausgabe. Die Golden-Werte wurden am 2026-11-24
byte-exakt gegen die frueheren Python-Referenzdekoder verifiziert.
Exit 0 = alle identisch.
"""
import hashlib
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TMP = os.path.join(ROOT, 'tools', 'out')
EXE = os.path.join(TMP, 'imgc_dump.exe')
ARC = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc')


def build():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(TMP, exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1', '-I', os.path.join(ROOT, 'runtime', 'gpu'),
           os.path.join(ROOT, 'runtime', 'gpu', 'imgc.c'),
           os.path.join(ROOT, 'tests', 'imgc_dump.c'), '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit('build fehlgeschlagen:\n' + p.stderr[-3000:])


def main():
    build()
    cases = json.load(open(os.path.join(ROOT, 'tests', 'golden', 'imgc.json'),
                           encoding='utf-8'))['cases']
    files = {}
    ok = True
    for c in cases:
        d = files.setdefault(c['fa'], open(os.path.join(ARC, c['fa']), 'rb').read())
        o = int(c['offset'], 16)
        s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
        src = os.path.join(TMP, c['name'] + '.xi')
        dst = os.path.join(TMP, c['name'] + '.rgb')
        open(src, 'wb').write(d[o:o + 0x48 + s2 + sz])
        env = dict(os.environ)
        env.update(c['env'])
        p = subprocess.run([EXE, src, dst], capture_output=True, text=True, env=env)
        if p.returncode != 0:
            print('FEHL %-14s imgc_dump exit %d' % (c['name'], p.returncode))
            ok = False
            continue
        w, h = [int(v) for v in p.stdout.split()]
        sha = hashlib.sha256(open(dst, 'rb').read()).hexdigest()
        good = (w, h, sha) == (c['w'], c['h'], c['rgb_sha256'])
        ok &= good
        print('%s %-14s %dx%d' % ('OK  ' if good else 'FEHL', c['name'], w, h))
    print('ERGEBNIS:', 'OK' if ok else 'FEHLER')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
