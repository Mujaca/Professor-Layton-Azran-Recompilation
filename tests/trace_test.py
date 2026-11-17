"""Tests fuer Format, Reader und Analyse des Trace-Harness."""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

from recomp import trace as tracemod  # noqa: E402

TMP = os.path.join(ROOT, "out", "trace_test.ctrt")


def u8(v):
    return struct.pack("<B", v)


def u16(v):
    return struct.pack("<H", v)


def u32(v):
    return struct.pack("<I", v)


def write_trace(path, records):
    with open(path, "wb") as f:
        f.write(b"CTRTRC01")
        for rec in records:
            if rec[0] == "block":
                f.write(b"B" + u32(rec[1]) + u8(rec[2]))
            elif rec[0] == "svc":
                f.write(b"S" + u32(rec[1]))
            elif rec[0] == "ipc":
                name = rec[1].encode("ascii")
                f.write(b"I" + u8(len(name)) + name + u16(rec[2]))
            elif rec[0] == "frame":
                f.write(b"F" + u32(rec[1]))
            elif rec[0] == "event":
                f.write(b"E" + u32(rec[1]) + u32(rec[2]) + u32(rec[3]))


def main():
    os.makedirs(os.path.dirname(TMP), exist_ok=True)
    recs = [
        ("block", 0x100000, 0),
        ("block", 0x100010, 0),
        ("svc", 0x32),
        ("ipc", "fs:USER", 0x0801),
        ("frame", 0),
        ("block", 0x100974, 1),
        ("block", 0x100000, 0),          # Duplikat
        ("event", 1, 0x100020, 0xE1A00000),
        ("frame", 1),
    ]
    write_trace(TMP, recs)

    got = list(tracemod.read(TMP))
    assert len(got) == len(recs), (len(got), len(recs))
    assert got[2] == ("svc", 0x32), got[2]
    assert got[3] == ("ipc", "fs:USER", 0x0801), got[3]

    entries = tracemod.entry_points(TMP)
    addrs = [e[0] for e in entries]
    assert addrs == [0x100000, 0x100010, 0x100974], addrs
    assert entries[2][2] is True, entries[2]

    st = tracemod.stats(TMP)
    assert st["unique_blocks"] == 3, st["unique_blocks"]
    assert st["frames"] == 2, st["frames"]
    assert st["top_svc"] == [(0x32, 1)], st["top_svc"]
    assert st["top_ipc"] == [["fs:USER#0x0801", 1]], st["top_ipc"]

    # diff: zweiter Trace weicht an Position 1 ab
    other = os.path.join(ROOT, "out", "trace_test2.ctrt")
    write_trace(other, [("block", 0x100000, 0), ("block", 0x100014, 0)])
    d = tracemod.diff(TMP, other)
    assert d["first_divergence"]["index"] == 1, d
    print("trace_test: alle Pruefungen ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
