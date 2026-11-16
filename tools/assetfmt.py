"""Format-Analyse der RomFS-Assets (nur Header lesen, nicht alles extrahieren)."""
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nokeys
nokeys.install()
from pyctr.type.cci import CCIReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "Professor Layton and the Azran Legacy (Europe).3ds")

TARGETS = [
    "/lt6/arc/lt6_a.fa",
    "/lt6/arc/lt6_b.fa",
    "/lt6/arc/lt6_uk.fa",
    "/lt6/mov/m001.moflex",
    "/lt6/snd/uk/product.bcsar",
    "/lt6/snd/uk/stream/BGM_004.dspadpcm.bcstm",
    "/lt6/ctr/uk/manual.cfa",
    "/lt6/ctr/version.cfg",
    "/lt6/ctr/uk/boxicon.ctpk",
]

def dump(path, head):
    print(f"\n=== {path}  ({len(head)} bytes gelesen)")
    print("  hex  :", head[:48].hex(" "))
    s = re.findall(rb'[\x20-\x7E]{4,}', head[:64])
    print("  start:", b" | ".join(s)[:200])
    strs = re.findall(rb'[\x20-\x7E]{6,}', head)
    if strs:
        print(f"  strings in den ersten {len(head)} Bytes: {len(strs)}")
        for x in strs[:12]:
            print("     ", x.decode('ascii')[:90])

with CCIReader(ROM, assume_decrypted=True) as cci:
    rf = cci.contents[0].romfs
    for t in TARGETS:
        try:
            with rf.open(t) as fh:
                head = fh.read(4096)
        except Exception as e:
            print(f"\n=== {t}: {type(e).__name__}: {e}")
            continue
        dump(t, head)
