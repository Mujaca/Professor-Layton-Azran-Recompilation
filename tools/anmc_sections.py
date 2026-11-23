"""Parse die ANMC-Sektionen mit bekannten Strides und dumpt alle Felder."""
import os
import re
import struct
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'tools/out/anmc_res.bin')


def f32(v):
    return struct.unpack('<f', struct.pack('<I', v))[0]


def main():
    d = open(BIN, 'rb').read()
    ss = [s.decode() for s in re.findall(rb'[ -~]{3,}', d)]
    h2s = {}
    for s in ss:
        h2s[zlib.crc32(s.encode()) & 0xFFFFFFFF] = s

    def dump(title, start, n, stride, nfloat):
        print('\n== %s  @%04X n=%d stride=0x%X' % (title, start, n, stride))
        for i in range(n):
            o = start + i * stride
            h = struct.unpack_from('<I', d, o)[0]
            raw = struct.unpack_from('<I', d, o + 4)[0]
            name = h2s.get(h, '%08X' % h)
            vals = [f32(struct.unpack_from('<I', d, o + 8 + 4 * k)[0])
                    for k in range(nfloat)]
            print('%04X %-22s raw=%08X  %s'
                  % (o, name, raw,
                     ' '.join('%9.3f' % v for v in vals)))
            u = [struct.unpack_from('<I', d, o + 8 + 4 * k)[0]
                 for k in range(nfloat)]
            print('     %-22s      u32: %s'
                  % ('', ' '.join('%08X' % v for v in u)))

    dump('node-geom (0x694)', 0x694, 18, 0x20, 6)
    dump('dur1 (0x8D4)', 0x8D4, 8, 0x14, 4)
    dump('dur2 (0x974)', 0x974, 22, 0x14, 4)
    dump('btr-geom (0xB2C)', 0xB2C, 21, 0x30, 10)
    dump('sound-geom (0xF1C)', 0xF1C, 8, 0x30, 10)
    dump('try_www (0x109C)', 0x109C, 1, 0x30, 10)


if __name__ == '__main__':
    main()
