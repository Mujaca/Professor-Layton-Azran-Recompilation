"""menu_*.xq: Record-Tabelle dekodieren (24-Byte-Records).

Layout (belegt: 17 Records * 0x18 = 408 Byte Huffman-Body von menu_logo):
  u32  name_offset   (monoton steigend; Offset in einen Namens-/Param-Pool)
  u32  name_hash     (CRC32-artiger Ressourcenname)
  u16  frame_start   \\
  u16  frame_end      /  [start,end); die Intervalle kacheln die Timeline
  u16  p0..p5        (6 Event-Parameter, objekttypabhaengig)

  python tools/xq_fields.py
"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc

SEQ = os.path.join(ROOT, 'out', 'fa_a', 'seq')
RSIZE = 0x18


def records(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<H', d, 4)[0]
    body = _imgc.decomp(d[0x20:])
    out = []
    for i in range(min(n, len(body) // RSIZE)):
        o = i * RSIZE
        noff, h = struct.unpack_from('<II', body, o)
        f = struct.unpack_from('<8H', body, o + 8)
        out.append((noff, h, f))
    return n, len(body), out


def main():
    files = sorted(glob.glob(os.path.join(SEQ, 'menu_*.xq')))
    print('%-18s %4s %6s %5s %6s %6s %7s' % (
        'datei', 'n', 'len', 'recs', 'tmin', 'tmax', 'luecken'))
    for p in files:
        n, ln, rs = records(p)
        iv = sorted((f[0], f[1]) for _, _, f in rs if f[1] > f[0])
        gaps = 0
        cur = None
        for a, b in iv:
            if cur is not None and a > cur:
                gaps += a - cur
            cur = b if cur is None else max(cur, b)
        mono = all(rs[i][0] <= rs[i + 1][0] for i in range(len(rs) - 1))
        print('%-18s %4d %6d %5d %6d %6d %7d  mono_off=%s' % (
            os.path.basename(p), n, ln, len(rs), iv[0][0], iv[-1][1],
            gaps, mono))

    p = os.path.join(SEQ, 'menu_logo.xq')
    n, ln, rs = records(p)
    print('\nmenu_logo.xq: n=%d len=%d' % (n, ln))
    print('  #  name_off hash      start  end   p0 p1 p2 p3 p4 p5')
    for i, (noff, h, f) in enumerate(rs):
        print('  %2d %8d %08X %5d %4d   %2d %2d %2d %2d %2d %2d'
              % (i, noff, h, f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                 f[7]))
    print('\nNach start sortiert (Kette, 0-Luecke?):')
    prev = 0
    for noff, h, f in sorted(rs, key=lambda r: (r[2][0], -r[2][1])):
        if f[1] == 0:
            continue
        mark = '' if f[0] == prev else '  <-- LUECKE'
        print('   %4d..%-4d hash=%08X%s' % (f[0], f[1], h, mark))
        prev = max(prev, f[1])

    p = os.path.join(SEQ, 'menu_title.xq')
    n, ln, rs = records(p)
    print('\nmenu_title.xq: n=%d len=%d' % (n, ln))
    print('  #  name_off hash      start  end   p0 p1 p2 p3 p4 p5')
    for i, (noff, h, f) in enumerate(rs):
        print('  %2d %8d %08X %5d %4d   %2d %2d %2d %2d %2d %2d'
              % (i, noff, h, f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                 f[7]))

    p = os.path.join(SEQ, 'menu_quest.xq')
    n, ln, rs = records(p)
    print('\nmenu_quest.xq: n=%d len=%d (Beispiel: erste 6 Records)' % (n, ln))
    for i, (noff, h, f) in enumerate(rs[:6]):
        print('  %2d %8d %08X %5d %4d   %2d %2d %2d %2d %2d %2d'
              % (i, noff, h, f[0], f[1], f[2], f[3], f[4], f[5], f[6],
                 f[7]))


if __name__ == '__main__':
    main()
