"""Bildvergleich Software- gegen Hardware-Renderer (M9) auf denselben Gast-Frames.

  python tools/scene.py froenborg --renderer soft --tag soft --every 50
  python tools/scene.py froenborg --renderer hw   --tag hw   --every 50
  python tools/rendercmp.py scratch/scene/froenborg-soft scratch/scene/froenborg-hw
  python tools/rendercmp.py A.png B.png --out scratch/cmp

Vergleicht gleichnamige Bilder (seq_*.png, ziel.png) zweier Ordner bzw. zwei
einzelne Bilder im Port-Layout 400x480 (oben 400x240, unten 320x240 ab x=40).
Je Bild und Screen: mittlere absolute Abweichung je Kanal (R/G/B), Anteil
Pixel mit max. Kanalabweichung > 8 und > 32, Maximalabweichung und die
Cluster grosser Abweichungen (> 32, 8-Nachbarschaft nach 3x3-Dilatation) mit
Bounding-Box. Einzelne Kantenpixel (Rasterregeln, Rundung) bilden keine
grossen Cluster; ein Cluster ab --cluster-min Pixeln gilt als Befund.

Ausgabe (--out, Standard: <B>/../<name>-cmp): cmp.txt (Tabelle), cmp.json,
heat_<bild>.png (Abweichung als Heatmap, schwarz = gleich) und
sheet_<bild>.png (A | B | Heatmap nebeneinander) fuer Bilder mit Befund oder
alle (--sheets all). Spielbilder gehoeren nur nach scratch/ oder out/.

Exit 0 immer (Messwerkzeug); --fail-cluster N: Exit 1, wenn ein Bild einen
Cluster >= N Pixel hat.
"""
import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

# Port-Layout (runtime/main.c, Screenshot 400x480): oben 400x240, unten
# 320x240 zentriert.
SCREENS = {"top": (0, 0, 400, 240), "bottom": (40, 240, 320, 240)}
T_SMALL = 8
T_LARGE = 32


def load(path):
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB")).astype(np.int16)


def screen_views(a):
    if a.shape[:2] != (480, 400):
        return {"full": a}
    return {k: a[y:y + h, x:x + w] for k, (x, y, w, h) in SCREENS.items()}


def clusters(mask, min_px):
    """Zusammenhaengende Gebiete (8-Nachbarschaft) der dilatierten Maske;
    gezaehlt werden die Originalpixel je Gebiet."""
    h, w = mask.shape
    dil = mask.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            dil[max(0, dy):h + min(0, dy), max(0, dx):w + min(0, dx)] |= \
                mask[max(0, -dy):h - max(0, dy), max(0, -dx):w - max(0, dx)]
    lab = np.zeros((h, w), np.int32)
    out = []
    ys, xs = np.nonzero(dil)
    n = 0
    for y0, x0 in zip(ys, xs):
        if lab[y0, x0]:
            continue
        n += 1
        stack = [(y0, x0)]
        lab[y0, x0] = n
        pts = []
        while stack:
            y, x = stack.pop()
            pts.append((y, x))
            for dy in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and dil[yy, xx] and not lab[yy, xx]:
                        lab[yy, xx] = n
                        stack.append((yy, xx))
        py = np.array([p[0] for p in pts])
        px = np.array([p[1] for p in pts])
        orig = mask[py, px]
        cnt = int(orig.sum())
        if cnt >= min_px:
            py, px = py[orig], px[orig]           # Bounding-Box ohne Dilatation
            out.append(dict(pixels=cnt, x0=int(px.min()), y0=int(py.min()),
                            x1=int(px.max()), y1=int(py.max())))
    out.sort(key=lambda c: -c["pixels"])
    return out


def compare(a, b, min_px):
    res = {}
    for (name, va), vb in zip(screen_views(a).items(), screen_views(b).values()):
        d = np.abs(va - vb)
        dm = d.max(axis=2)
        n = dm.size
        big = dm > T_LARGE
        res[name] = dict(
            mad=[round(float(d[..., c].mean()), 3) for c in range(3)],
            p8=round(100.0 * float((dm > T_SMALL).sum()) / n, 3),
            p32=round(100.0 * float(big.sum()) / n, 3),
            max=int(dm.max()),
            clusters=clusters(big, min_px) if big.any() else [])
    return res


def heatmap(a, b):
    """Max. Kanalabweichung: 0 schwarz, 1..8 dunkelblau, 9..32 gelb, >32 rot
    (Helligkeit steigt mit der Abweichung)."""
    dm = np.abs(a - b).max(axis=2)
    h = np.zeros(dm.shape + (3,), np.uint8)
    s = dm > 0
    h[s] = (0, 0, 96)
    m = (dm > T_SMALL) & (dm <= T_LARGE)
    h[m] = (200, 200, 0)
    l = dm > T_LARGE
    h[..., 0] = np.where(l, np.clip(128 + dm, 0, 255), h[..., 0])
    h[l, 1] = 0
    h[l, 2] = 0
    return h


def pairs(a, b):
    a, b = Path(a), Path(b)
    if a.is_file() and b.is_file():
        return [(a.stem, a, b)]
    names = sorted(p.name for p in list(a.glob("*.png")) + list(a.glob("*.bmp"))
                   if not p.name.startswith(("heat_", "sheet_")))
    return [(Path(n).stem, a / n, b / n) for n in names if (b / n).is_file()]


def fmt_row(img, scr, r):
    cl = r["clusters"]
    big = ("%d (%d px @%d,%d-%d,%d)" % (len(cl), cl[0]["pixels"], cl[0]["x0"], cl[0]["y0"],
                                         cl[0]["x1"], cl[0]["y1"])) if cl else "0"
    return "%-14s %-6s %6.2f %6.2f %6.2f %7.3f %7.3f %4d  %s" % (
        img, scr, r["mad"][0], r["mad"][1], r["mad"][2], r["p8"], r["p32"], r["max"], big)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("a", help="Ordner/Bild A (Referenz, Software-Pfad)")
    ap.add_argument("b", help="Ordner/Bild B (Hardware-Pfad)")
    ap.add_argument("--out", default="", help="Ausgabeordner")
    ap.add_argument("--cluster-min", type=int, default=16,
                    help="Mindestgroesse (Pixel > 32) eines gemeldeten Clusters")
    ap.add_argument("--sheets", choices=["none", "findings", "all"], default="findings")
    ap.add_argument("--fail-cluster", type=int, default=0)
    a = ap.parse_args(argv)
    ps = pairs(a.a, a.b)
    if not ps:
        print("keine gemeinsamen Bilder")
        return 1
    out = Path(a.out) if a.out else Path(a.b).resolve().parent / (Path(a.b).name + "-cmp")
    out.mkdir(parents=True, exist_ok=True)
    rows = ["%-14s %-6s %6s %6s %6s %7s %7s %4s  %s" % (
        "bild", "screen", "madR", "madG", "madB", "%>8", "%>32", "max", "cluster>=%d (groesster)" % a.cluster_min)]
    allres = {}
    worst = 0
    for name, pa, pb in ps:
        ia, ib = load(pa), load(pb)
        if ia.shape != ib.shape:
            rows.append("%-14s Groesse verschieden %s %s" % (name, ia.shape, ib.shape))
            continue
        r = compare(ia, ib, a.cluster_min)
        allres[name] = r
        for scr, rr in r.items():
            rows.append(fmt_row(name, scr, rr))
            if rr["clusters"]:
                worst = max(worst, rr["clusters"][0]["pixels"])
        finding = any(rr["clusters"] for rr in r.values())
        if a.sheets == "all" or (a.sheets == "findings" and finding):
            hm = heatmap(ia, ib)
            Image.fromarray(hm).save(out / ("heat_%s.png" % name))
            sheet = np.concatenate([ia.astype(np.uint8), ib.astype(np.uint8), hm], axis=1)
            Image.fromarray(sheet).save(out / ("sheet_%s.png" % name))
    # Zusammenfassung je Screen ueber alle Bilder
    rows.append("")
    for scr in sorted({s for r in allres.values() for s in r}):
        rs = [r[scr] for r in allres.values() if scr in r]
        mad = np.mean([np.mean(x["mad"]) for x in rs])
        rows.append("%-6s %d Bilder: mittl. MAD %.3f, max %%>8 %.3f, max %%>32 %.3f, max %d, "
                    "Bilder mit Cluster %d" % (
                        scr, len(rs), mad, max(x["p8"] for x in rs), max(x["p32"] for x in rs),
                        max(x["max"] for x in rs), sum(1 for x in rs if x["clusters"])))
    text = "\n".join(rows) + "\n"
    (out / "cmp.txt").write_text(text, encoding="utf-8")
    (out / "cmp.json").write_text(json.dumps(allres, indent=1), encoding="utf-8")
    print(text, end="")
    print("Ausgabe: %s" % out)
    if a.fail_cluster and worst >= a.fail_cluster:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
