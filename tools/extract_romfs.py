"""Entpackt das komplette RomFS nach out/romfs/ (fuer den fs-Shim der Runtime).

  python tools/extract_romfs.py                    # Repo-Layout
  python tools/extract_romfs.py --rom D.3ds --out O  # -> O/romfs, O/romfs_index.json
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import nokeys

nokeys.install()
from pyctr.type.cci import CCIReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "Professor Layton and the Azran Legacy (Europe).3ds")


def walk(node, prefix, out):
    for name, e in node["contents"].items():
        p = prefix + "/" + e["name"]
        if e.get("type") == "dir":
            walk(e, p, out)
        else:
            out.append((p, e["size"]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rom", default=ROM)
    ap.add_argument("--out", default=os.path.join(ROOT, "out"))
    a = ap.parse_args()
    dest = os.path.join(a.out, "romfs")
    with CCIReader(a.rom, assume_decrypted=True) as cci:
        rf = cci.contents[0].romfs
        files = []
        walk(rf._tree_root, "", files)
        files.sort()
        index = {}
        for i, (p, size) in enumerate(files):
            rel = p.lstrip("/")
            target = os.path.join(dest, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with rf.open(p) as fh:
                data = fh.read()
            with open(target, "wb") as out:
                out.write(data)
            index[p] = {"size": size, "path": rel}
            if i % 50 == 0:
                print("%3d/%d %s" % (i, len(files), rel), flush=True)
    with open(os.path.join(a.out, "romfs_index.json"), "w", encoding="utf-8") as f:
        json.dump(index, f, indent=1)
    print("fertig:", len(files), "Dateien")


if __name__ == "__main__":
    main()
