"""Schneller (numpy) Level-5-IMGC00-Dekoder fuer Analyse-Zwecke.

Unterstuetzt 24bpp/32bpp (RGB bzw. ABGR) sowie optional 8bpp (Graustufen)
und 4bpp (Nibble -> Graustufen) ohne Palette.
"""
import os
import struct
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _imgc

BIT_COORDS = np.array([(0, 1), (1, 0), (0, 2), (2, 0), (0, 4), (4, 0)],
                      dtype=np.int32)


def parse_header(d, o):
    if o + 0x48 > len(d) or d[o:o + 6] != b'IMGC00':
        return None
    bits = d[o + 0x0D]
    bpt = struct.unpack_from('<h', d, o + 0x0E)[0]
    w, h = struct.unpack_from('<hh', d, o + 0x10)
    toff = struct.unpack_from('<i', d, o + 0x1C)[0]
    s1, s2, sz = struct.unpack_from('<iii', d, o + 0x34)
    return dict(off=o, bits=bits, bpt=bpt, w=w, h=h, toff=toff,
                s1=s1, s2=s2, sz=sz)


def swizzle_src(w, h):
    """Return src-index array (h,w) fuer den IMGC-Z-Order-Swizzle."""
    w8 = (w + 7) & ~7
    yy, xx = np.mgrid[0:h, 0:w]
    idx = yy * w8 + xx
    macro = idx // 64
    wt = w8 // 8
    sx = (macro % wt) * 8
    sy = (macro // wt) * 8
    for j, (bx, by) in enumerate(BIT_COORDS):
        bit = ((idx >> j) & 1).astype(np.int32)
        sx = sx ^ (bit * bx)
        sy = sy ^ (bit * by)
    return sy * w8 + sx


def decode_buf(d, o):
    """Dekodiert IMGC am Offset o aus Puffer d -> (w,h,rgb uint8 [h,w,3],
    alpha uint8 [h,w] oder None)."""
    hd = parse_header(d, o)
    if hd is None:
        return None
    w, h, bits, bpt = hd['w'], hd['h'], hd['bits'], hd['bpt']
    if w <= 0 or h <= 0 or bits not in (4, 8, 16, 24, 32):
        return None
    bpp = bits // 8
    if bpp == 0:
        bpp = 0.5
    if bpt != int(64 * bpp):
        return None
    if hd['s1'] <= 0 or hd['sz'] <= 0:
        return None
    base = o + hd['toff']
    tab = _imgc.decomp(d[base:base + hd['s1']])
    tex = _imgc.decomp(d[base + hd['s2']:base + hd['s2'] + hd['sz']])
    if not tab or not tex:
        return None
    epos, elem = 0, 2
    if len(tab) >= 2 and struct.unpack_from('<H', tab, 0)[0] == 0x453:
        epos, elem = 8, 4
    n = (len(tab) - epos) // elem
    if n <= 0:
        return None
    if elem == 2:
        e = np.frombuffer(tab, dtype='<u2', count=n, offset=epos).astype(np.int64)
    else:
        e = np.frombuffer(tab, dtype='<u4', count=n, offset=epos).astype(np.int64)
    bad = e >= 0x8000
    e = np.where(bad, 0, e)
    bpt_i = int(bpt)
    ntile_max = len(tex) // bpt_i
    e = np.clip(e, 0, ntile_max)
    tilebytes = np.zeros((n, bpt_i), dtype=np.uint8)
    valid = (~bad) & (e < ntile_max)
    if valid.any():
        src = np.frombuffer(tex, dtype=np.uint8)[:ntile_max * bpt_i]
        src = src.reshape(ntile_max, bpt_i)
        tilebytes[valid] = src[e[valid]]
    pic = tilebytes.reshape(-1)
    srcidx = swizzle_src(w, h).reshape(-1)
    if bits == 32:
        need = (srcidx * 4 + 4)
        ok = need <= len(pic)
        rgb = np.zeros((w * h, 3), dtype=np.uint8)
        alpha = np.zeros(w * h, dtype=np.uint8)
        srcidx_c = np.where(ok, srcidx, 0)
        px = np.frombuffer(pic.tobytes(), dtype=np.uint8).reshape(-1, 4)[srcidx_c]
        rgb[:, 0] = np.where(ok, px[:, 3], 0)
        rgb[:, 1] = np.where(ok, px[:, 2], 0)
        rgb[:, 2] = np.where(ok, px[:, 1], 0)
        alpha = np.where(ok, px[:, 0], 0)
        return w, h, rgb.reshape(h, w, 3), alpha.reshape(h, w)
    if bits == 24:
        ok = (srcidx * 3 + 3) <= len(pic)
        rgb = np.zeros((w * h, 3), dtype=np.uint8)
        srcidx_c = np.where(ok, srcidx, 0)
        px = np.frombuffer(pic.tobytes(), dtype=np.uint8).reshape(-1, 3)[srcidx_c]
        rgb[:, 0] = np.where(ok, px[:, 2], 0)
        rgb[:, 1] = np.where(ok, px[:, 1], 0)
        rgb[:, 2] = np.where(ok, px[:, 0], 0)
        return w, h, rgb.reshape(h, w, 3), None
    if bits == 8:
        ok = srcidx < len(pic)
        srcidx_c = np.where(ok, srcidx, 0)
        g = np.frombuffer(pic.tobytes(), dtype=np.uint8)[srcidx_c]
        g = np.where(ok, g, 0).astype(np.uint8)
        rgb = np.repeat(g[:, None], 3, axis=1).reshape(h, w, 3)
        return w, h, rgb, None
    if bits == 16:
        # 16bpp: als BGR565? -> hier nur grob, nicht fuer Menue relevant
        return None
    # 4bpp: Nibble -> Graustufe*17
    ok = srcidx < len(pic)
    srcidx_c = np.where(ok, srcidx, 0)
    b = np.frombuffer(pic.tobytes(), dtype=np.uint8)[srcidx_c]
    lo = (b & 0x0F).astype(np.uint8) * 17
    hi = ((b >> 4) & 0x0F).astype(np.uint8) * 17
    g = np.where(ok, lo, 0).astype(np.uint8)
    rgb = np.repeat(g[:, None], 3, axis=1).reshape(h, w, 3)
    return w, h, rgb, None


if __name__ == '__main__':
    fa = sys.argv[1]
    off = int(sys.argv[2], 0)
    d = open(fa, 'rb').read()
    r = decode_buf(d, off)
    print('decode', fa, off, None if r is None else (r[0], r[1]))
