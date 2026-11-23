"""Library: menu/title.xc (XPCK) -> Szene, .xi ETC1-Decode, .prm-Quad.

Reines Analyse-Modul; keine Runtime-Aenderung.
"""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import _imgc
import _atest_xpck as X

FA = os.path.join(ROOT, 'out/romfs/lt6/arc/lt6_b.fa')
TITLE_OFF = 0x0CD691E0
TITLE_LEN = 0xD9E90

MODS = ((2, 8, -2, -8), (5, 17, -5, -17), (9, 29, -9, -29),
        (13, 42, -13, -42), (18, 60, -18, -60), (24, 80, -24, -80),
        (33, 106, -33, -106), (47, 183, -47, -183))

_cache = {}


def fa():
    if 'fa' not in _cache:
        _cache['fa'] = np.memmap(FA, dtype=np.uint8, mode='r')
    return _cache['fa']


def entries():
    if 'ents' not in _cache:
        d = open(FA, 'rb').read()
        _cache['ents'] = X.parse(d, TITLE_OFF, TITLE_LEN, _imgc.decomp)[1]
    return _cache['ents']


def blob(name):
    for e in entries():
        if e['name'] == name:
            o = TITLE_OFF + e['off']
            return np.asarray(fa()[o:o + e['size']]).tobytes()
    raise KeyError(name)


def lz10(d):
    size = ((d[0] >> 3) | (d[1] << 5) | (d[2] << 13) | (d[3] << 21))
    p = 4
    out = bytearray()
    while len(out) < size:
        fl = d[p]
        p += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if fl & (0x80 >> bit):
                v = (d[p] << 8) | d[p + 1]
                p += 2
                n = (v >> 12) + 3
                disp = (v & 0xFFF) + 1
                for _ in range(n):
                    if len(out) >= size:
                        break
                    out.append(out[-disp])
            else:
                out.append(d[p])
                p += 1
    return bytes(out)


def _etc1_block(buf, p, out, w, ox, oy):
    """PICA-ETC1-Block, Layout wie Azahar Pica::Texture::ETC1Tile.

    raw ist ein u64_le (byte0 = LSB).  Die Felder liegen bei
    0..15 table_subindex, 16..31 negation, 32 flip, 33 diff,
    34..36 table2, 37..39 table1, 40..63 Basisfarben.
    """
    raw = int.from_bytes(buf[p:p + 8], 'little')
    flip = (raw >> 32) & 1
    diff = (raw >> 33) & 1
    t2 = (raw >> 34) & 7
    t1 = (raw >> 37) & 7
    if diff:
        def b5(v):
            v &= 0x1F
            return (v << 3) | (v >> 2)
        r1 = (raw >> 59) & 31
        g1 = (raw >> 51) & 31
        b1 = (raw >> 43) & 31
        r2 = b5(max(0, min(31, r1 + _s3((raw >> 56) & 7))))
        g2 = b5(max(0, min(31, g1 + _s3((raw >> 48) & 7))))
        b2 = b5(max(0, min(31, b1 + _s3((raw >> 40) & 7))))
        r1, g1, b1 = b5(r1), b5(g1), b5(b1)
    else:
        r1, g1, b1 = ((raw >> 60) & 15) * 17, ((raw >> 52) & 15) * 17, ((raw >> 44) & 15) * 17
        r2, g2, b2 = ((raw >> 56) & 15) * 17, ((raw >> 48) & 15) * 17, ((raw >> 40) & 15) * 17
    for y in range(4):
        for x in range(4):
            texel = 4 * x + y
            xx = y if flip else x
            ti = t1 if xx < 2 else t2
            m = MODS[ti][(raw >> texel) & 1]
            if (raw >> (16 + texel)) & 1:
                m = -m
            br, bg, bb = (r2, g2, b2) if xx >= 2 else (r1, g1, b1)
            o = ((oy + y) * w + ox + x) * 3
            out[o] = max(0, min(255, br + m))
            out[o + 1] = max(0, min(255, bg + m))
            out[o + 2] = max(0, min(255, bb + m))


def _s3(v):
    return v - 8 if v & 4 else v


def xi_rgb(name):
    """Decode .xi (IMGC, ETC1) -> (w,h,np.uint8[h,w,3])."""
    key = 'xi:' + name
    if key in _cache:
        return _cache[key]
    b = blob(name)
    w, h = struct.unpack_from('<HH', b, 0x10)
    s1, s2, sz = struct.unpack_from('<3i', b, 0x34)
    tab = _imgc.decomp(b[0x48:0x48 + s1])
    tex = _imgc.decomp(b[0x48 + s2:0x48 + s2 + sz])
    cols, rows = (w + 7) // 8, (h + 7) // 8
    n = cols * rows
    ents = struct.unpack_from('<%dH' % (len(tab) // 2), tab, 0)
    etc = bytearray(n * 32)
    for pp in range(min(n, len(ents))):
        e = ents[pp]
        if e == 0xFFFF:
            continue
        s = e
        if s * 32 + 32 > len(tex):
            continue
        etc[pp * 32:pp * 32 + 32] = tex[s * 32:s * 32 + 32]
    out = bytearray(w * h * 3)
    for ty in range(rows):
        for tx in range(cols):
            base = (ty * cols + tx) * 32
            for sy in range(2):
                for sx in range(2):
                    _etc1_block(etc, base + (sy * 2 + sx) * 8, out, w,
                                tx * 8 + sx * 4, ty * 8 + sy * 4)
    a = np.frombuffer(bytes(out), dtype=np.uint8).reshape(h, w, 3).copy()
    _cache[key] = (w, h, a)
    return w, h, a


def prm_info(name):
    """.prm -> Liste von Submeshes: dict(verts=Nx9, idx, mat, tbl)."""
    b = blob(name)
    p = b.find(b'XPVB')
    ao, ae, vo, st, vc = struct.unpack_from('<HHHHI', b, p + 4)
    dec = lz10(b[p + vo:])
    nf = st // 4
    verts = np.frombuffer(dec[:vc * st], dtype='<f4').reshape(vc, nf)
    pi = b.find(b'XPVI')
    out = dict(xpvb=p, stride=st, count=vc, xpvi=pi, verts=verts,
               raw=b[p + vo:p + vo + 16])
    # Submeshes/Indizes aus XPVI ableiten (spaeter verfeinert)
    if pi >= 0:
        hdr = struct.unpack_from('<8I', b, pi + 4)
        out['xpvi_hdr'] = hdr
    return out


def node_str(name):
    b = blob(name)
    return [s.decode('latin1') for s in b.split(b'\0') if len(s) >= 4]
