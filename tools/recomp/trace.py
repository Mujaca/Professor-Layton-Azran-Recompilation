"""Lesen und Auswerten der Trace-Dateien (Format "CTRTRC01").

Record-Typen (Little-Endian):
  'B' u32 pc,  u8 thumb        Blockstart (JIT-Uebersetzung / Ausfuehrung)
  'S' u32 imm                  SVC-Nummer
  'I' u8 len, name[len], u16 cmd   IPC-Request an einen Service
  'F' u32 index                Frame-/Buffer-Swap
  'E' u32 kind, u32 a, u32 b   freies Ereignis (z. B. unimplemented)
"""
import argparse
import collections
import json
import struct

MAGIC = b"CTRTRC01"


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != MAGIC:
        raise ValueError("%s: kein CTRTRC01-Trace (Kopf: %r)" % (path, data[:8]))
    off = 8
    n = len(data)
    while off < n:
        tag = data[off:off + 1]
        off += 1
        if tag == b"B":
            if off + 5 > n:
                break
            pc, thumb = struct.unpack_from("<IB", data, off)
            off += 5
            yield ("block", pc, thumb)
        elif tag == b"S":
            if off + 4 > n:
                break
            (imm,) = struct.unpack_from("<I", data, off)
            off += 4
            yield ("svc", imm)
        elif tag == b"I":
            if off + 1 > n:
                break
            ln = data[off]
            off += 1
            name = data[off:off + ln].decode("ascii", "replace")
            off += ln
            (cmd,) = struct.unpack_from("<H", data, off)
            off += 2
            yield ("ipc", name, cmd)
        elif tag == b"F":
            if off + 4 > n:
                break
            (idx,) = struct.unpack_from("<I", data, off)
            off += 4
            yield ("frame", idx)
        elif tag == b"E":
            if off + 12 > n:
                break
            kind, a, b = struct.unpack_from("<III", data, off)
            off += 12
            yield ("event", kind, a, b)
        else:
            raise ValueError("%s: unbekannter Record-Tag %r bei 0x%X" % (path, tag, off - 1))


def entry_points(path):
    """Seeds fuer den Scanner: alle im Trace gesehenen Blockstarts."""
    seen = set()
    out = []
    for rec in read(path):
        if rec[0] != "block":
            continue
        addr, thumb = rec[1], bool(rec[2])
        key = (addr, thumb)
        if key in seen:
            continue
        seen.add(key)
        out.append((addr, "trace", thumb))
    return out


def stats(path):
    counts = collections.Counter()
    blocks = set()
    svcs = collections.Counter()
    services = collections.Counter()
    frames = 0
    events = collections.Counter()

    for rec in read(path):
        kind = rec[0]
        counts[kind] += 1
        if kind == "block":
            blocks.add((rec[1], rec[2]))
        elif kind == "svc":
            svcs[rec[1]] += 1
        elif kind == "ipc":
            services[(rec[1], rec[2])] += 1
        elif kind == "frame":
            frames += 1
        elif kind == "event":
            events[rec[1]] += 1

    return {
        "records": dict(counts),
        "unique_blocks": len(blocks),
        "frames": frames,
        "top_svc": svcs.most_common(15),
        "top_ipc": [["%s#0x%04X" % (s, c), n] for (s, c), n in services.most_common(20)],
        "events": dict(events),
        "blocks": sorted(blocks),
    }


def coverage(path, res):
    """Vergleicht Trace-Blocks mit dem Scanner-Ergebnis."""
    known = set()
    for addr in res.visited:
        known.add(addr & ~1)
    traced = set(addr & ~1 for addr, _ in stats(path)["blocks"])
    new = traced - known
    return {
        "traced_blocks": len(traced),
        "known_blocks": len(traced & known),
        "new_blocks": len(new),
        "new_pct": round(100.0 * len(new) / max(1, len(traced)), 2),
        "examples": sorted(new)[:20],
    }


def diff(a, b):
    """Vergleicht zwei Traces blockweise (Reihenfolge + Mengen)."""
    seq_a = [(pc & ~1, th) for kind, pc, th in (r for r in read(a) if r[0] == "block")]
    seq_b = [(pc & ~1, th) for kind, pc, th in (r for r in read(b) if r[0] == "block")]
    first = None
    for i, (x, y) in enumerate(zip(seq_a, seq_b)):
        if x != y:
            first = (i, x, y)
            break
    if first is None and len(seq_a) != len(seq_b):
        first = (min(len(seq_a), len(seq_b)),
                 seq_a[min(len(seq_a), len(seq_b)) - 1] if seq_a else None,
                 seq_b[min(len(seq_a), len(seq_b)) - 1] if seq_b else None)
    metrics = {
        "blocks_a": len(seq_a),
        "blocks_b": len(seq_b),
        "only_a": len(set(seq_a) - set(seq_b)),
        "only_b": len(set(seq_b) - set(seq_a)),
        "first_divergence": None if first is None else {
            "index": first[0],
            "a": None if first[1] is None else "0x%08X (%s)" % (first[1][0], "thumb" if first[1][1] else "arm"),
            "b": None if first[2] is None else "0x%08X (%s)" % (first[2][0], "thumb" if first[2][1] else "arm"),
        },
    }
    return metrics


def main(argv=None):
    ap = argparse.ArgumentParser(prog="recomp.trace")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p_stats = sub.add_parser("stats")
    p_stats.add_argument("trace")
    p_stats.add_argument("--json")

    p_cov = sub.add_parser("coverage")
    p_cov.add_argument("trace")
    p_cov.add_argument("--in", dest="inp", required=True)
    p_cov.add_argument("--prologues", action="store_true")
    p_cov.add_argument("--json")

    p_diff = sub.add_parser("diff")
    p_diff.add_argument("a")
    p_diff.add_argument("b")

    args = ap.parse_args(argv)

    if args.cmd == "stats":
        st = stats(args.trace)
        blocks = st.pop("blocks")
        print(json.dumps(st, indent=2))
        print("\nerste Blockadressen:", ", ".join("0x%08X" % a for a, _ in blocks[:10]))
        if args.json:
            st["blocks"] = ["0x%08X" % a for a, _ in blocks]
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(st, f, indent=2)

    elif args.cmd == "coverage":
        from . import isa
        from . import scan as scanmod

        data = open(args.inp, "rb").read()
        res = scanmod.scan(data, [(isa.ARM_BASE, "entry", False)],
                           follow_prologues=args.prologues)
        cov = coverage(args.trace, res)
        print(json.dumps(cov, indent=2))
        if args.json:
            with open(args.json, "w", encoding="utf-8") as f:
                json.dump(cov, f, indent=2)

    else:
        print(json.dumps(diff(args.a, args.b), indent=2))


if __name__ == "__main__":
    main()
