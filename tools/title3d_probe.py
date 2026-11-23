"""menu/title.xc: Eintraege listen + Texturen testweise dekodieren.

`title.xc` ist die XPCK des 3D-Titels. Dieses Tool listet Namen/Magics und
versucht jeden Eintrag als IMGC00 zu dekodieren (w/h/Format), damit klar
wird, welche Textur den Titelgrund bildet.

Aufruf: python tools/title3d_probe.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc  # noqa: E402

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_b.fa')
OFF = 0x0CD691E0
LEN = 0xD9E90


def entries():
    d = open(FA, 'rb').read()[OFF:OFF + LEN]
    fc1, fc2 = d[4], d[5]
    count = ((fc2 & 0xF) << 8) | fc1
    info_off = struct.unpack_from('<H', d, 6)[0] << 2
    name_off = struct.unpack_from('<H', d, 8)[0] << 2
    data_off = struct.unpack_from('<H', d, 0xA)[0] << 2
    name_size = struct.unpack_from('<H', d, 0xE)[0] << 2
    names = _imgc.decomp(d[name_off:name_off + name_size])
    out = []
    for i in range(count):
        e = info_off + i * 12
        h, no, t1, t2, z1, z2 = struct.unpack_from('<IHHHBB', d, e)
        s = names[no:names.find(b'\0', no)].decode('latin1')
        fo = data_off + (((z1 << 16) | t1) << 2)
        sz = ((z2 << 16) | t2)
        out.append((s, fo, sz, d[fo:fo + 6]))
    return d, out


def main():
    d, ents = entries()
    print('entries=%d  xc=%d Bytes' % (len(ents), len(d)))
    for s, fo, sz, magic in ents:
        tag = magic[:6]
        note = ''
        if tag == b'IMGC00':
            w = struct.unpack_from('<h', d, fo + 0x10)[0]
            h = struct.unpack_from('<h', d, fo + 0x12)[0]
            bd = d[fo + 0x0D]
            note = 'IMGC %dx%d bpp%d' % (w, h, bd)
        print('%-24s off=%06X size=%7d  %-6s %s'
              % (s, fo, sz, tag.decode('latin1', errors='replace'), note))


if __name__ == '__main__':
    main()
