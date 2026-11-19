"""framecheck: out/shot.bmp gegen reports/azahar_topscreen.png vergleichen.

Rein mit der Standardbibliothek: BMP- und PNG-Decoder (8 Bit, nicht
interlaced), Nearest-Neighbor-Downscale, dominante Farben, Perzentile der
Pixeldifferenz und explizite Platzhalter-Erkennung (288.054 Byte).
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path
from typing import List, Sequence, Tuple

from . import common

RGB = Tuple[int, int, int]


def read_bmp(path: Path):
    data = path.read_bytes()
    if len(data) < 54 or data[:2] != b"BM":
        common.fail("kein BMP: %s" % path)
    offset = struct.unpack_from("<I", data, 10)[0]
    hdr = struct.unpack_from("<IiiHHIIiiII", data, 14)
    width, height, bpp, compression = hdr[1], hdr[2], hdr[4], hdr[5]
    top_down = height < 0
    height = abs(height)
    if compression != 0 or bpp not in (24, 32):
        common.fail("BMP-Format nicht unterstuetzt (bpp=%d comp=%d)" %
                    (bpp, compression))
    rows: List[List[RGB]] = []
    stride = ((width * bpp + 31) // 32) * 4
    for y in range(height):
        src_y = y if top_down else (height - 1 - y)
        base = offset + src_y * stride
        row = []
        step = bpp // 8
        for x in range(width):
            i = base + x * step
            b, g, r = data[i], data[i + 1], data[i + 2]
            row.append((r, g, b))
        rows.append(row)
    return width, height, rows, len(data)


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def read_png(path: Path):
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        common.fail("kein PNG: %s" % path)
    pos = 8
    width = height = bitdepth = colortype = interlace = 0
    idat = b""
    palette: List[RGB] = []
    while pos + 8 <= len(data):
        length = struct.unpack_from(">I", data, pos)[0]
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bitdepth, colortype, _comp, _filt, interlace = \
                struct.unpack(">IIBBBBB", chunk)
        elif ctype == b"PLTE":
            palette = [(chunk[i], chunk[i + 1], chunk[i + 2])
                       for i in range(0, len(chunk) - 2, 3)]
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if bitdepth != 8 or interlace != 0:
        common.fail("PNG nur 8 Bit/nicht interlaced unterstuetzt "
                    "(depth=%d interlace=%d)" % (bitdepth, interlace))
    if colortype == 0:
        channels = 1
    elif colortype == 2:
        channels = 3
    elif colortype == 4:
        channels = 2
    elif colortype == 6:
        channels = 4
    elif colortype == 3:
        channels = 1
    else:
        common.fail("PNG-Farbtyp %d nicht unterstuetzt" % colortype)
    raw = zlib.decompress(idat)
    bpp = channels
    stride = width * channels
    prev = bytearray(stride)
    rows: List[List[RGB]] = []
    p = 0
    for _y in range(height):
        ftype = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if ftype == 1:
                line[i] = (line[i] + a) & 0xFF
            elif ftype == 2:
                line[i] = (line[i] + b) & 0xFF
            elif ftype == 3:
                line[i] = (line[i] + ((a + b) >> 1)) & 0xFF
            elif ftype == 4:
                line[i] = (line[i] + _paeth(a, b, c)) & 0xFF
        prev = line
        row = []
        for x in range(width):
            i = x * channels
            if colortype == 0:
                v = line[i]
                row.append((v, v, v))
            elif colortype == 2:
                row.append((line[i], line[i + 1], line[i + 2]))
            elif colortype == 4:
                v = line[i]
                row.append((v, v, v))
            elif colortype == 6:
                row.append((line[i], line[i + 1], line[i + 2]))
            else:  # palette
                idx = line[i]
                row.append(palette[idx] if idx < len(palette) else (0, 0, 0))
        rows.append(row)
    return width, height, rows, len(data)


def _downscale(rows: Sequence[Sequence[RGB]], w: int, h: int,
               out_w: int = 96, out_h: int = 54) -> List[RGB]:
    out: List[RGB] = []
    for y in range(out_h):
        sy = min(h - 1, int((y + 0.5) * h / out_h))
        for x in range(out_w):
            sx = min(w - 1, int((x + 0.5) * w / out_w))
            out.append(rows[sy][sx])
    return out


def _dominant(px: Sequence[RGB], top: int = 5):
    counts = {}
    for r, g, b in px:
        key = (r >> 3, g >> 3, b >> 3)
        counts[key] = counts.get(key, 0) + 1
    total = max(1, len(px))
    out = []
    for (r, g, b), n in sorted(counts.items(), key=lambda kv: -kv[1])[:top]:
        out.append(("#%02X%02X%02X" % (r << 3, g << 3, b << 3),
                    100.0 * n / total))
    return out


def _saturation(px: Sequence[RGB]) -> float:
    if not px:
        return 0.0
    sat = sum(1 for r, g, b in px if max(r, g, b) - min(r, g, b) > 20)
    return 100.0 * sat / len(px)


def _percentile(values: Sequence[int], p: float) -> int:
    if not values:
        return 0
    s = sorted(values)
    i = min(len(s) - 1, max(0, int(round((p / 100.0) * (len(s) - 1)))))
    return s[i]


def compare(shot: Path, ref: Path) -> dict:
    sw, sh, srows, ssize = read_bmp(shot)
    rw, rh, rrows, rsize = read_png(ref)
    sd = _downscale(srows, sw, sh)
    rd = _downscale(rrows, rw, rh)
    diffs = []
    total = 0
    for a, b in zip(sd, rd):
        d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
        diffs.append(d)
        total += d
    mean = total / max(1, len(diffs))
    sat_s = _saturation(sd)
    dom_s = _dominant(sd)
    unique = len({(r >> 3, g >> 3, b >> 3) for r, g, b in sd})
    placeholder = (ssize == 288054) or (dom_s and dom_s[0][1] > 95.0) or \
        (unique <= 4)
    return {
        "shot": str(shot), "ref": str(ref),
        "shot_size": [sw, sh], "ref_size": [rw, rh],
        "shot_bytes": ssize, "ref_bytes": rsize,
        "mean_abs_diff": mean,
        "p50_diff": _percentile(diffs, 50),
        "p90_diff": _percentile(diffs, 90),
        "saturation_shot": sat_s, "saturation_ref": _saturation(rd),
        "dominant_shot": dom_s, "dominant_ref": _dominant(rd),
        "unique_colors_shot": unique,
        "placeholder": placeholder,
    }


def render(r: dict) -> List[str]:
    lines = ["framecheck: %s vs. %s" % (r["shot"], r["ref"])]
    lines.append("  Groesse: Port %dx%d (%d B), Referenz %dx%d (%d B)" % (
        r["shot_size"][0], r["shot_size"][1], r["shot_bytes"],
        r["ref_size"][0], r["ref_size"][1], r["ref_bytes"]))
    lines.append("  Pixeldifferenz (96x54): Mittel %.1f, p50 %d, p90 %d" % (
        r["mean_abs_diff"], r["p50_diff"], r["p90_diff"]))
    lines.append("  Saettigung: Port %.1f%%, Referenz %.1f%%" % (
        r["saturation_shot"], r["saturation_ref"]))
    lines.append("  Dominante Farben Port: " + ", ".join(
        "%s %.1f%%" % (c, p) for c, p in r["dominant_shot"]))
    lines.append("  Dominante Farben Referenz: " + ", ".join(
        "%s %.1f%%" % (c, p) for c, p in r["dominant_ref"]))
    if r["placeholder"]:
        lines.append("  PLATZHALTER ERKANNT: %d B (400x240, Port-Menue ohne "
                     "Spiel-Render)" % r["shot_bytes"])
    else:
        lines.append("  Kein Port-Platzhalter (Groesse/Farbvielfalt weichen ab).")
    return lines


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    p = argparse.ArgumentParser(
        prog="framecheck",
        description="Screenshot gegen Azahar-Referenz vergleichen")
    p.add_argument("--shot", default=str(common.ROOT / "out" / "shot.bmp"))
    p.add_argument("--ref", default=str(common.ROOT / "reports" /
                                        "azahar_topscreen.png"))
    p.add_argument("--json-out")
    p.add_argument("--fail-on-placeholder", action="store_true")
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args(argv)
    shot = Path(args.shot)
    ref = Path(args.ref)
    common.check_file(shot, "Screenshot", "out\\port.exe out\\romfs --shot out\\shot.bmp")
    common.check_file(ref, "Referenzbild")
    r = compare(shot, ref)
    common.print_summary(render(r), 30 if not args.verbose else 100)
    if args.json_out:
        Path(args.json_out).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json_out).write_text(json.dumps(r, indent=2,
                                                  ensure_ascii=False),
                                       encoding="utf-8")
    if args.fail_on_placeholder and r["placeholder"]:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
