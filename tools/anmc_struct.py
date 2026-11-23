"""Beleg-Dump der ANMC00-Struktur (RES.bin aus menu/uk/title_b.xa)."""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'tools/out/anmc_res.bin')


def f32(d, o):
    return struct.unpack('<f', d[o:o + 4])[0]


def main():
    d = open(BIN, 'rb').read()
    print('ANMC00 dekomprimiert: %d Bytes (0x%X)' % (len(d), len(d)))
    print('\n[Header]')
    for i in range(0, 0x6C, 16):
        print('%04X  %s' % (i, ' '.join('%02X' % b for b in d[i:i + 16])))
    print('\n[Sektionen (durch Hash-Scan belegt)]')
    secs = [
        (0x0064, 'root title_b_menu + Identity/UV-Matrix'),
        (0x0158, 'Keyframe-Liste #CHP (18 Events, variable Laenge 0x3C..0x50)'),
        (0x064C, 'Hash-Tabelle der 18 Event-Kommandos (18*u32=0x48)'),
        (0x0694, 'Node-Geometrie (18 Records, stride 0x20)'),
        (0x08D4, 'Track-Tabelle A (8 Records, stride 0x14)'),
        (0x0974, 'Track-Tabelle B (22 Records, stride 0x14)'),
        (0x0B2C, 'Button-Platzierungen btr01/btr/s01/s (21 Records, stride 0x30)'),
        (0x0F1C, 'Sound/try-Platzierungen (8 Records, stride 0x30)'),
        (0x109C, 'try_www-Platzierung (1 Record, stride 0x30)'),
        (0x10CC, 'String-Tabelle (64 Strings)'),
    ]
    for o, t in secs:
        print('  @%04X  %s' % (o, t))

    print('\n[Platzierungs-Record: 4B Hash, u32, 10*float]')
    print('  Felder: f1,f2 = Anker (Bildschirm); f4,f5..f7,f8 = lokale BBox;')
    for off, name in ((0xB2C, 'btr01_continue_off'),
                      (0xCAC, 'btr_continue_off'),
                      (0xDCC, 'pointer'),
                      (0xF1C, 'sound1_off'),
                      (0x103C, 'try_off'),
                      (0x109C, 'try_www')):
        h = struct.unpack_from('<I', d, off)[0]
        raw = struct.unpack_from('<I', d, off + 4)[0]
        f = [f32(d, off + 8 + 4 * k) for k in range(10)]
        print('\n  @%04X %-22s hash=%08X raw=%08X' % (off, name, h, raw))
        print('    f = [%s]' % ', '.join('%g' % v for v in f))
        # Rechteck: Anker + lokale BBox
        x = f[1] + f[4]
        y = f[2] + f[5]
        w = f[7] - f[4]
        hh = f[8] - f[5]
        print('    -> screen rect (x=%g, y=%g, w=%g, h=%g)' % (x, y, w, hh))

    print('\n[Node-Geometrie 0x694, stride 0x20]')
    for i in range(18):
        o = 0x694 + i * 0x20
        h = struct.unpack_from('<I', d, o)[0]
        raw = struct.unpack_from('<I', d, o + 4)[0]
        f = [f32(d, o + 8 + 4 * k) for k in range(6)]
        print('  @%04X hash=%08X raw=%08X  lx=%g ly=%g w=%g h=%g'
              % (o, h, raw, f[0], f[1], f[3], f[4]))

    print('\n[Event-Keyframes 0x158.. (cmd_hash, frame, 0x24DC7F35, '
          'sprite_hash, #CHP, 9 floats)]')
    for off in (0x160, 0x19C, 0x1D8, 0x250, 0x28C, 0x37C, 0x55C):
        h = struct.unpack_from('<I', d, off)[0]
        frame = struct.unpack_from('<I', d, off + 4)[0]
        f = [f32(d, off + 0x14 + 4 * k) for k in range(9)]
        print('  @%04X hash=%08X frame=%3d  %s'
              % (off, h, frame, ' '.join('%g' % v for v in f)))

    print('\n[CRC32-Hashes der Namen]')
    ss = [s.decode() for s in re.findall(rb'[ -~]{3,}', d)]
    for s in ss:
        if s.startswith('#') or s in ('title_b_menu', 'pointer', 'try_www'):
            print('  %08X  %s' % (zlib.crc32(s.encode()) & 0xFFFFFFFF, s))

    print('\n[Beleg: 000.xi ist ein Tile-Muster-Atlas]')
    # u16-Tabelle/Textur aus dem IMGC-Header ableiten
    import _imgc
    import _atest_xpck as X
    fa = open(os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_uk.fa'), 'rb').read()
    cnt, ents = X.parse(fa, 0x01526DC0, 0x9278, _imgc.decomp)
    e = [x for x in ents if x['name'] == '000.xi'][0]
    o = 0x01526DC0 + e['off']
    toff = struct.unpack_from('<i', fa, o + 0x1C)[0]
    s1, s2, sz = struct.unpack_from('<iii', fa, o + 0x34)
    tab = _imgc.decomp(fa[o + toff:o + toff + s1])
    tex = _imgc.decomp(fa[o + toff + s2:o + toff + s2 + sz])
    import numpy as np
    u16 = np.frombuffer(tab, dtype='<u2')
    print('  Tabelle %d B -> %d u16, unique %d, max(nicht FFFF)=%d'
          % (len(tab), len(u16), len(np.unique(u16)),
             int(u16[u16 != 0xFFFF].max())))
    print('  Textur %d B = %d Tiles x 256 B (8x8 RGBA) -> Tiles werden '
          'vielfach wiederverwendet' % (len(tex), len(tex) // 256))


if __name__ == '__main__':
    main()
