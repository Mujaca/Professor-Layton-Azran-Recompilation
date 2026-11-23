"""Ordnet IMGC-Offsets den Dateinamen im .fa zu."""
import sys

sys.path.insert(0, 'tools')
import fa_index

TARGETS = [61255048, 262068120, 65661048, 66064632, 216281204, 159095284,
           206698200, 228497140, 21136784, 32129416, 5282620, 185149876,
           68035108, 143443980, 59656816, 193250340, 193004540, 24411912,
           194022360, 66998020, 75559760]


def main():
    for fa in ('out/romfs/lt6/arc/lt6_b.fa', 'out/romfs/lt6/arc/lt6_uk.fa'):
        d, data_off, toc, files, folders = fa_index.parse(fa)
        ents = sorted((off, size, name) for name, base, crc in files
                      for (off, size) in [toc.get(crc, (0, 0))]
                      if size)
        for t in TARGETS:
            for off, size, name in ents:
                if off <= t < off + size:
                    print('%-14s %-10d -> %s (+0x%X)' % (
                        fa.split('/')[-1], t, name, t - off))
                    break


if __name__ == '__main__':
    main()
