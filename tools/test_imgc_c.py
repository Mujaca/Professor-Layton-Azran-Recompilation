"""Regressionstest fuer runtime/imgc.c (ETC1 + lineare Tile-Ordnung).

  python tools\\test_imgc_c.py

Vergleicht den C-Dekoder byte-exakt gegen die Python-Referenzdekoder
(_atest_botscan.decode_linear fuer 24/32 bpp, title3d_lib.xi_rgb fuer ETC1)
und misst den Bottom-Diff (linear vs. alte Z-Order) gegen die Referenz.
Exit  beck 0 = alle Vergleiche identisch, 1 = Abweichung.
"""
import os
import struct
import subprocess
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _atest_botscan as BS
import _atest_imgc_np as D
import _atest_xpck as X
import _imgc
import title3d_lib as L

FA_B = os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc', 'lt6_b.fa')
EXE = os.path.join(ROOT, 'tools', 'out', 'imgc_dump.exe')
TMP = os.path.join(ROOT, 'tools', 'out')
BOTTOM_OFF = 0x0CE43074


def build():
    import ziglang
    zig = os.path.join(os.path.dirname(ziglang.__file__), 'zig.exe')
    os.makedirs(TMP, exist_ok=True)
    cmd = [zig, 'cc', '-std=c11', '-O1', '-I', os.path.join(ROOT, 'runtime'),
           os.path.join(ROOT, 'runtime', 'imgc.c'),
           os.path.join(ROOT, 'tests', 'imgc_dump.c'), '-o', EXE]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit('build fehlgeschlagen:\n' + p.stderr[-3000:])
    print('build ok:', EXE)


def slice_imgc(d, off):
    s1, s2, sz = struct.unpack_from('<iii', d, off + 0x34)
    return d[off:off + 0x48 + s2 + sz]


def run_c(blob, name, env=None):
    src = os.path.join(TMP, name + '.xi')
    dst = os.path.join(TMP, name + '.rgb')
    open(src, 'wb').write(blob)
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run([EXE, src, dst], capture_output=True, text=True,
                       env=e)
    if p.returncode != 0:
        raise SystemExit('imgc_dump %s exit %d' % (name, p.returncode))
    w, h = [int(v) for v in p.stdout.split()]
    return w, h, open(dst, 'rb').read()


def cmp_bytes(a, b, label):
    if a == b:
        print('OK   %-34s %d Byte identisch' % (label, len(a)))
        return True
    n = min(len(a), len(b))
    diff = sum(1 for i in range(n) if a[i] != b[i]) + abs(len(a) - len(b))
    print('FEHL %-34s %d/%d Byte verschieden' % (label, diff, len(a)))
    return False


def main():
    build()
    ok = True
    d = open(FA_B, 'rb').read()

    # (a) 24 bpp, lineare Tile-Ordnung: title_b_bg
    blob = slice_imgc(d, BOTTOM_OFF)
    w, h, rgb = run_c(blob, 'title_b_bg')
    lin = BS.decode_linear(d, BOTTOM_OFF)
    ok &= cmp_bytes(rgb, lin.tobytes(), 'title_b_bg linear')

    # Diff gegen Bottom-Capture: linear vs. alte Z-Order
    cap = np.frombuffer(open(os.path.join(
        ROOT, 'reports', 'menu_official_bottom.bin'), 'rb').read(),
        dtype=np.uint8).reshape(240, 320, 3)
    _, _, sw = run_c(blob, 'title_b_bg_z', {'HLE_IMGC_ZORDER': '1'})
    for label, img in (('linear', rgb), ('z-order', sw)):
        a = np.frombuffer(img, dtype=np.uint8).reshape(240, 320, 3)
        vis = a[:223].astype(np.int16)
        dd = int((np.abs(vis - cap[:223]).sum(2) > 60).sum())
        mean = float(np.abs(vis - cap[:223]).mean())
        print('     bottom %-8s diff>60 %6d/71360  mean %.2f'
              % (label, dd, mean))
        if label == 'linear':
            linear_diff = dd
        else:
            z_diff = dd
    ok &= linear_diff < z_diff

    # (b) ETC1: alle 14 .xi aus menu/title.xc byte-exakt
    for e in L.entries():
        if e['magic'] != 'IMGC00':
            continue
        o = L.TITLE_OFF + e['off']
        if (d[o + 0x0D] != 4 or
                struct.unpack_from('<H', d, o + 0x0A)[0] != 0x011B):
            continue
        w2, h2, ref = L.xi_rgb(e['name'])
        _, _, got = run_c(slice_imgc(d, o), 'etc1_' + e['name'])
        ok &= cmp_bytes(got, ref.tobytes(),
                        'ETC1 %s (%dx%d)' % (e['name'], w2, h2))

    # (c) alte Z-Order bleibt per Schalter byte-identisch (32-bpp-Asset;
    #     ETC1 ist bewusst immer linear, siehe IMGC imageFormat 0x011B)
    duk = open(os.path.join(ROOT, 'out', 'romfs', 'lt6', 'arc',
                            'lt6_uk.fa'), 'rb').read()
    for e in X.parse(duk, 0x01428314, 0xFEAA8, _imgc.decomp)[1]:
        if e['name'] != '000.xi':
            continue
        o = 0x01428314 + e['off']
        _, _, got = run_c(slice_imgc(duk, o), 'z_000',
                          {'HLE_IMGC_ZORDER': '1'})
        ref = D.decode_buf(duk, o)
        ok &= cmp_bytes(got, ref[2].tobytes(), '000.xi Z-Order (Schalter)')

    print('ERGEBNIS:', 'OK' if ok else 'FEHLER')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
