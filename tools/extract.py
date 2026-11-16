"""Entpackt ExeFS + erstellt ein RomFS-Inventar fuer Professor Layton (Azran Legacy)."""
import sys, os, json, collections
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nokeys
nokeys.install()
from pyctr.type.cci import CCIReader

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROM = os.path.join(ROOT, "Professor Layton and the Azran Legacy (Europe).3ds")
OUT = os.path.join(ROOT, "out")
REP = os.path.join(ROOT, "reports")


def walk(node, prefix, files, dirs):
    for name, e in node["contents"].items():
        p = prefix + "/" + e["name"]
        if e.get("type") == "dir":
            dirs.append(p)
            walk(e, p, files, dirs)
        else:
            files.append((p, e.get("size", -1), e.get("offset", -1)))


def main():
    os.makedirs(os.path.join(OUT, "exefs"), exist_ok=True)
    os.makedirs(REP, exist_ok=True)

    with CCIReader(ROM, assume_decrypted=True) as cci:
        ncch = cci.contents[0]
        ex = ncch.exefs
        print("ExeFS Eintraege:")
        for name, entry in ex.entries.items():
            data = ex.get_data(entry, 0, entry.size)
            fn = os.path.join(OUT, "exefs", name.lstrip(".") + ".bin")
            with open(fn, "wb") as fh:
                fh.write(data)
            print(f"  {name:8s} {entry.size:>9,} bytes -> {os.path.relpath(fn, ROOT)}  first={data[:4].hex()}")

        rf = ncch.romfs
        files, dirs = [], []
        walk(rf._tree_root, "", files, dirs)
        print(f"\nRomFS: {len(files):,} Dateien in {len(dirs):,} Verzeichnissen, "
              f"{sum(s for _, s, _ in files)/2**20:,.1f} MiB")

        lay = sorted(files, key=lambda t: -t[1])
        with open(os.path.join(REP, "romfs_listing.tsv"), "w", encoding="utf-8") as fh:
            fh.write("path\tsize\toffset\n")
            for p, s, o in lay:
                fh.write(f"{p}\t{s}\t{o}\n")

        exts = collections.Counter()
        size_by_ext = collections.Counter()
        for p, s, _ in files:
            e = os.path.splitext(p)[1].lower() or "<none>"
            exts[e] += 1
            size_by_ext[e] += s

        print("\nTop-Verzeichnisse:")
        top = collections.Counter()
        for p, s, _ in files:
            parts = p.strip("/").split("/")
            key = "/".join(parts[:3])
            top[key] += s
        for k, v in top.most_common(20):
            print(f"  {v/2**20:>10,.1f} MiB  {k}")

        print("\nDateitypen (Top 25 nach Groesse):")
        for e, sz in size_by_ext.most_common(25):
            print(f"  {sz/2**20:>10,.1f} MiB  {exts[e]:>6,} x  {e}")

        print("\nGroesste Dateien:")
        for p, s, _ in lay[:20]:
            print(f"  {s/2**20:>9,.2f} MiB  {p}")

        json.dump({
            "files": len(files), "dirs": len(dirs),
            "bytes": sum(s for _, s, _ in files),
            "ext_count": dict(exts), "ext_bytes": dict(size_by_ext),
        }, open(os.path.join(REP, "romfs_summary.json"), "w"), indent=2)


if __name__ == "__main__":
    main()
