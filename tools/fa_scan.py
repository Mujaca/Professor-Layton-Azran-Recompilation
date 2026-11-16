"""Untersucht die Level-5 'ARC0'-Archive (.fa) auf Struktur, Namen und Kompression."""
import sys, os, re, math, collections, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nokeys
nokeys.install()
from pyctr.type.cci import CCIReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "Professor Layton and the Azran Legacy (Europe).3ds")


def entropy(b):
    if not b:
        return 0.0
    c = collections.Counter(b)
    n = len(b)
    return -sum((v / n) * math.log2(v / n) for v in c.values())


with CCIReader(ROM, assume_decrypted=True) as cci:
    rf = cci.contents[0].romfs
    for name in ("/lt6/arc/lt6_a.fa", "/lt6/arc/lt6_b.fa", "/lt6/arc/lt6_uk.fa"):
        with rf.open(name) as fh:
            head = fh.read(8 * 2**20)
            fh.seek(0, 2)
            size = fh.tell()

        words = " ".join(f"0x{struct.unpack_from('<I', head, i)[0]:X}" for i in range(0, 0x40, 4))
        print(f"\n=== {name}  ({size:,} bytes)")
        print("  header-u32:", words)
        txt = re.findall(rb'[\x20-\x7E]{6,}', head)
        print(f"  ASCII-Strings >= 6 in den ersten 8 MiB: {len(txt)}")
        for t in txt[:12]:
            print("    ", t.decode('ascii')[:100])

        for frac in (0.0, 0.25, 0.5, 0.75):
            with rf.open(name) as fh:
                fh.seek(int(size * frac))
                chunk = fh.read(65536)
            print(f"  Entropie @{frac*100:>3.0f}%: {entropy(chunk):.2f}")
