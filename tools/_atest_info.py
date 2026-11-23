"""Groesse/bpp/Name fuer ausgewaehlte IMGC-Offsets."""
import struct
import sys

sys.path.insert(0, 'tools')
import fa_index

T = [216281204, 68035108, 185149876, 194022360, 59656816, 143443980,
     193250340, 193004540, 61255048, 262068120, 24411912, 67225108,
     66998020, 75559760, 575736, 84091004]


def main():
    for fa in ('out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa'):
        d, data_off, toc, files, folders = fa_index.parse(fa)
        ents = sorted((off, size, name) for name, base, crc in files
                      for (off, size) in [toc.get(crc, (0, 0))] if size)
        for t in T:
            for off, size, name in ents:
                if off <= t < off + size:
                    w, h = struct.unpack_from('<hh', d, t + 0x10)
                    print('%-14s %-10d %4dx%-4d bits=%-2d  %s (+0x%X)'
                          % (fa.split('/')[-1], t, w, h, d[t + 0x0D], name,
                             t - off))
                    break


if __name__ == '__main__':
    main()
