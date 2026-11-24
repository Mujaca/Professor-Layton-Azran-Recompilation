"""Unabhaengiger MOFLEX-Parser (Layout wie FFmpeg libavformat/moflex.c).

Gibt je Stream die zusammengesetzten Frames aus: (frame_nr, groesse,
Liste der Teilpakete (datei_offset, laenge)). Nur Analyse, kein Projektcode.
"""
import sys


class BR:
    def __init__(self, d, pos):
        self.d, self.p, self.bitpos, self.last = d, pos, 0, 0

    def pop(self):
        if (self.bitpos & 7) == 0:
            self.last = self.d[self.p] << 24
            self.p += 1
        else:
            self.last = (self.last << 1) & 0xFFFFFFFF
        self.bitpos += 1
        return 1 if self.last & 0x80000000 else 0

    def pop_int(self, n):
        v = 0
        for _ in range(n):
            v = 2 * v + self.pop()
        return v

    def pop_length(self):
        n = 1
        while self.pop() == 0:
            n += 1
        return n


def var_byte(d, p):
    b = d[p]; p += 1
    if not b & 0x80:
        return b, p
    v = (b & 0x7F) << 7
    b = d[p]; p += 1
    if not b & 0x80:
        return v | b, p
    v = ((b & 0x7F) | v) << 7
    b = d[p]; p += 1
    if not b & 0x80:
        return v | b, p
    v = (((b & 0x7F) | v) << 7) | d[p]; p += 1
    return v, p


def parse(d, verbose=False):
    p = 0
    frames = {}       # stream -> list of (size, parts)
    cur = {}          # stream -> parts
    size = 0
    nblocks = 0
    while p + 2 <= len(d):
        pos = p
        if d[p:p + 2] == b"L2":
            p += 4
            p += 8                        # ts
            size = ((d[p] << 8) | d[p + 1]) + 1
            p += 2
            while True:
                t, p = var_byte(d, p)
                ss, p = var_byte(d, p)
                if t == 0:
                    p += ss
                    break
                p += ss
        if p >= len(d):
            break
        flags = d[p]; p += 1
        if flags & 2:
            p += 2
        nblocks += 1
        while p < pos + size and p < len(d):
            b = d[p]; p += 1
            if b == 0:
                break
            p -= 1
            br = BR(d, p)
            si = br.pop_int(br.pop_length())
            endframe = br.pop()
            if endframe:
                bits = br.pop_length(); br.pop_int(bits); br.pop()
                bits = br.pop_length(); br.pop_int(bits * 2 + 26)
            pkt = br.pop_int(13) + 1
            p = br.p
            cur.setdefault(si, []).append((p, pkt))
            p += pkt
            if endframe:
                parts = cur.pop(si)
                frames.setdefault(si, []).append((sum(l for _, l in parts), parts))
        if flags % 2 == 0:
            p = pos + size
    return frames, nblocks


if __name__ == "__main__":
    d = open(sys.argv[1], "rb").read()
    fr, nb = parse(d)
    print("Bloecke:", nb, "Streams:", {k: len(v) for k, v in fr.items()})
    vs = fr.get(0, [])
    for i, (sz, parts) in enumerate(vs[:60], 1):
        print("Frame %2d: %6d Byte in %d Teil(en): %s" % (
            i, sz, len(parts), " ".join("@%X+%X" % (o, l) for o, l in parts[:4])))
