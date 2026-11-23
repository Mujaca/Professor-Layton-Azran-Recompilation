/* Dekoder fuer Level-5 IMGC00 (.xi).
   Verfahren: 1 = LZ10, 2 = Huffman4Bit, 3 = Huffman8Bit, 4 = RLE, 0 = roh.
   Formate: 0x0100/0x0103 = 32/24 bpp (BGR(A) -> RGB), 0x011B = ETC1,
   0x011C = 8 bpp Graustufe.
   Tile-Reihenfolge ist linear (Kachelzeile fuer Kachelzeile, 8x8 raster);
   HLE_IMGC_ZORDER=1 stellt die alte Z-Order (master_get) wieder her. */
#include "imgc.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define IMGC_FMT_ETC1 0x011Bu
#define IMGC_FMT_L8   0x011Cu

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Einmal gelesen (nicht im Hot Path): lineare Tile-Ordnung ist Default. */
static int tile_order_linear(void) {
    static int lin = -1;
    if (lin < 0) {
        const char *z = getenv("HLE_IMGC_ZORDER");
        lin = (z == NULL || *z == 0 || *z == '0') ? 1 : 0;
    }
    return lin;
}

static uint32_t lvl5_size(const uint8_t *in, uint32_t inlen) {
    if (inlen < 4u) return 0u;
    return ((uint32_t)in[0] >> 3) | ((uint32_t)in[1] << 5) |
           ((uint32_t)in[2] << 13) | ((uint32_t)in[3] << 21);
}

static int lvl5_rle(const uint8_t *in, uint32_t inlen,
                    uint8_t *out, uint32_t need) {
    uint32_t size = lvl5_size(in, inlen), p = 4u, o = 0u;
    if (size == 0u || size > need) return -1;
    while (o < size && p < inlen) {
        uint32_t f = in[p++];
        if (f & 0x80u) {
            uint32_t r = (f & 0x7Fu) + 3u;
            uint8_t v;
            if (p >= inlen) return -1;
            v = in[p++];
            if (o + r > need) r = need - o;
            memset(out + o, v, r);
            o += r;
        } else {
            uint32_t n = f + 1u;
            if (p + n > inlen) return -1;
            if (o + n > need) n = need - o;
            memcpy(out + o, in + p, n);
            o += n;
            p += n;
        }
    }
    return (int)o;
}

static int lvl5_huff(const uint8_t *in, uint32_t inlen,
                     uint8_t *out, uint32_t need, int bits) {
    uint32_t size = lvl5_size(in, inlen), p = 4u, o = 0u, i = 0u;
    uint32_t ts, root;
    const uint8_t *tree;
    int32_t pos, nxt = 0, code = 0, nib = -1;
    if (size == 0u || size > need) return -1;
    if (p + 2u > inlen) return -1;
    ts = in[p++];
    root = in[p++];
    if (p + ts * 2u > inlen) return -1;
    tree = in + p;
    p += ts * 2u;
    pos = (int32_t)root;
    while (o < size) {
        int bit, dir, leaf;
        int32_t idx;
        if ((i & 31u) == 0u) {
            if (p + 4u > inlen) return -1;
            code = (int32_t)rd32(in + p);
            p += 4u;
        }
        nxt += ((pos & 0x3F) << 1) + 2;
        bit = (int)((code >> (31 - (int)(i & 31u))) & 1);
        dir = bit ? 1 : 2;
        leaf = (int)((pos >> (5 + dir)) & 1);
        idx = nxt - dir;
        if (idx < 0 || (uint32_t)idx >= ts * 2u) return -1;
        pos = (int32_t)tree[idx];
        if (leaf) {
            if (bits == 4) {
                if (nib < 0) {
                    nib = pos & 0xF;
                } else {
                    out[o++] = (uint8_t)(nib | ((pos & 0xF) << 4));
                    nib = -1;
                }
            } else {
                out[o++] = (uint8_t)pos;
            }
            pos = (int32_t)root;
            nxt = 0;
        }
        i++;
        if (i > inlen * 8u) return -1;
    }
    return (int)o;
}

static int lvl5_lz10(const uint8_t *in, uint32_t inlen,
                     uint8_t *out, uint32_t need) {
    uint32_t size = lvl5_size(in, inlen), p = 4u, o = 0u;
    if (size == 0u || size > need) return -1;
    while (o < size) {
        uint32_t flags, bit;
        if (p >= inlen) return -1;
        flags = in[p++];
        for (bit = 0u; bit < 8u && o < size; bit++) {
            if (flags & (0x80u >> bit)) {
                uint32_t v, len, disp, k;
                if (p + 2u > inlen) return -1;
                v = ((uint32_t)in[p] << 8) | in[p + 1u];
                p += 2u;
                len = (v >> 12) + 3u;
                disp = (v & 0xFFFu) + 1u;
                if (disp > o) return -1;
                for (k = 0u; k < len && o < size; k++) {
                    out[o] = out[o - disp];
                    o++;
                }
            } else {
                if (p >= inlen) return -1;
                out[o++] = in[p++];
            }
        }
    }
    return (int)o;
}

static int lvl5_decomp(const uint8_t *in, uint32_t inlen,
                       uint8_t *out, uint32_t cap) {
    uint32_t m;
    if (inlen < 4u) return -1;
    m = in[0] & 7u;
    if (m == 0u) {
        uint32_t n = inlen - 4u;
        if (n > cap) n = cap;
        memcpy(out, in + 4u, n);
        return (int)n;
    }
    if (m == 1u) return lvl5_lz10(in, inlen, out, cap);
    if (m == 2u) return lvl5_huff(in, inlen, out, cap, 4);
    if (m == 3u) return lvl5_huff(in, inlen, out, cap, 8);
    if (m == 4u) return lvl5_rle(in, inlen, out, cap);
    return -1;
}

static void master_get(uint32_t point, uint32_t width, int *px, int *py) {
    static const int bx[6] = {0, 1, 0, 2, 0, 4};
    static const int by[6] = {1, 0, 2, 0, 4, 0};
    uint32_t macro = point / 64u;
    uint32_t wt = (width + 7u) / 8u;
    int x = (int)((macro % wt) * 8u);
    int y = (int)((macro / wt) * 8u);
    int j;
    for (j = 0; j < 6; j++) {
        if ((point >> j) & 1u) {
            x ^= bx[j];
            y ^= by[j];
        }
    }
    *px = x;
    *py = y;
}

/* --- ETC1 (PICA-Layout, IMGC imageFormat 0x011B, 32 B/Tile) ------------- */

static const int etc1_mod[8][4] = {
    {2, 8, -2, -8}, {5, 17, -5, -17}, {9, 29, -9, -29},
    {13, 42, -13, -42}, {18, 60, -18, -60}, {24, 80, -24, -80},
    {33, 106, -33, -106}, {47, 183, -47, -183}
};

static int etc1_s3(int v) {
    return (v & 4) ? v - 8 : v;
}

static int etc1_clamp(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void etc1_block(const uint8_t *blk, uint8_t *rgb,
                       int w, int ox, int oy) {
    uint64_t raw = 0u;
    int i, x, y;
    int flip, diff, t2, t1;
    int r1, g1, b1, r2, g2, b2;
    for (i = 7; i >= 0; i--) raw = (raw << 8) | (uint64_t)blk[i];
    flip = (int)((raw >> 32) & 1u);
    diff = (int)((raw >> 33) & 1u);
    t2 = (int)((raw >> 34) & 7u);
    t1 = (int)((raw >> 37) & 7u);
    if (diff) {
        int q1r = (int)((raw >> 59) & 31u);
        int q1g = (int)((raw >> 51) & 31u);
        int q1b = (int)((raw >> 43) & 31u);
        int q2r = q1r + etc1_s3((int)((raw >> 56) & 7u));
        int q2g = q1g + etc1_s3((int)((raw >> 48) & 7u));
        int q2b = q1b + etc1_s3((int)((raw >> 40) & 7u));
        if (q2r < 0) q2r = 0; else if (q2r > 31) q2r = 31;
        if (q2g < 0) q2g = 0; else if (q2g > 31) q2g = 31;
        if (q2b < 0) q2b = 0; else if (q2b > 31) q2b = 31;
        r1 = (q1r << 3) | (q1r >> 2);
        g1 = (q1g << 3) | (q1g >> 2);
        b1 = (q1b << 3) | (q1b >> 2);
        r2 = (q2r << 3) | (q2r >> 2);
        g2 = (q2g << 3) | (q2g >> 2);
        b2 = (q2b << 3) | (q2b >> 2);
    } else {
        r1 = (int)((raw >> 60) & 15u) * 17;
        g1 = (int)((raw >> 52) & 15u) * 17;
        b1 = (int)((raw >> 44) & 15u) * 17;
        r2 = (int)((raw >> 56) & 15u) * 17;
        g2 = (int)((raw >> 48) & 15u) * 17;
        b2 = (int)((raw >> 40) & 15u) * 17;
    }
    for (y = 0; y < 4; y++) {
        for (x = 0; x < 4; x++) {
            int texel = 4 * x + y;
            int xx = flip ? y : x;
            int ti = (xx < 2) ? t1 : t2;
            int m = etc1_mod[ti][(int)((raw >> texel) & 1u)];
            uint32_t o;
            if ((raw >> (16 + texel)) & 1u) m = -m;
            o = (uint32_t)((oy + y) * w + ox + x) * 3u;
            rgb[o + 0u] = (uint8_t)etc1_clamp(((xx < 2) ? r1 : r2) + m);
            rgb[o + 1u] = (uint8_t)etc1_clamp(((xx < 2) ? g1 : g2) + m);
            rgb[o + 2u] = (uint8_t)etc1_clamp(((xx < 2) ? b1 : b2) + m);
        }
    }
}

/* --- Tile-Heap -> Bild -------------------------------------------------- */

static void blit_linear(const uint8_t *pic, uint32_t piclen, uint8_t *rgb,
                        int w, int h, int bpp) {
    int cols = (w + 7) / 8;
    int rows = (h + 7) / 8;
    int ty, tx, iy, ix;
    for (ty = 0; ty < rows; ty++) {
        for (tx = 0; tx < cols; tx++) {
            uint32_t base = ((uint32_t)ty * (uint32_t)cols + (uint32_t)tx) *
                            64u * (uint32_t)bpp;
            for (iy = 0; iy < 8; iy++) {
                int py = ty * 8 + iy;
                if (py >= h) break;
                for (ix = 0; ix < 8; ix++) {
                    int px = tx * 8 + ix;
                    uint32_t s;
                    uint8_t *d;
                    if (px >= w) break;
                    s = base + ((uint32_t)iy * 8u + (uint32_t)ix) *
                        (uint32_t)bpp;
                    if (s + (uint32_t)bpp > piclen) continue;
                    d = rgb + ((size_t)py * (size_t)w + (size_t)px) * 3u;
                    if (bpp == 4) {
                        d[0] = pic[s + 3u];
                        d[1] = pic[s + 2u];
                        d[2] = pic[s + 1u];
                    } else if (bpp == 1) {
                        d[0] = d[1] = d[2] = pic[s];
                    } else {
                        d[0] = pic[s + 2u];
                        d[1] = pic[s + 1u];
                        d[2] = pic[s + 0u];
                    }
                }
            }
        }
    }
}

static void blit_zorder(const uint8_t *pic, uint32_t piclen, uint8_t *rgb,
                        int w, int h, int bpp) {
    uint32_t w8 = ((uint32_t)w + 7u) & ~7u;
    int x, y;
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            int sx, sy;
            uint32_t o;
            size_t i = ((size_t)y * (size_t)w + (size_t)x) * 3u;
            master_get((uint32_t)y * w8 + (uint32_t)x, w8, &sx, &sy);
            o = ((uint32_t)sy * w8 + (uint32_t)sx) * (uint32_t)bpp;
            if (o + (uint32_t)bpp <= piclen) {
                if (bpp == 4) {
                    rgb[i + 0u] = pic[o + 3u];
                    rgb[i + 1u] = pic[o + 2u];
                    rgb[i + 2u] = pic[o + 1u];
                } else if (bpp == 1) {
                    rgb[i + 0u] = rgb[i + 1u] = rgb[i + 2u] = pic[o];
                } else {
                    rgb[i + 0u] = pic[o + 2u];
                    rgb[i + 1u] = pic[o + 1u];
                    rgb[i + 2u] = pic[o + 0u];
                }
            } else {
                rgb[i + 0u] = rgb[i + 1u] = rgb[i + 2u] = 0u;
            }
        }
    }
}

int imgc_decode(const uint8_t *src, uint32_t srclen,
                uint8_t *rgb, int *out_w, int *out_h) {
    uint32_t bpt, toff, t1, t2, isz, declen, declen2, epos = 0u, count, k;
    int bits, w, h, elem = 2, got, got2, bpp, fmt, etc1 = 0, linear;
    uint8_t *tab = NULL, *img = NULL, *pic = NULL;
    uint32_t piclen, tiles, cols, rows, m;
    if (srclen < 0x48u || memcmp(src, "IMGC00", 6) != 0) return -1;
    fmt = (int)rd16(src + 0x0A);
    bits = src[0x0D];
    w = (int)rd16(src + 0x10);
    h = (int)rd16(src + 0x12);
    bpt = rd16(src + 0x0E);
    if (w <= 0 || h <= 0) return -1;
    if (fmt == IMGC_FMT_ETC1) {
        if (bits != 4 || bpt != 32u) return -1;
        etc1 = 1;
        bpp = 0;
    } else if (fmt == IMGC_FMT_L8) {
        if (bits != 8 || bpt != 64u) return -1;
        bpp = 1;
    } else {
        if (bits != 24 && bits != 32) return -1;
        bpp = bits / 8;
        if (bpt != (uint32_t)(64 * bpp)) return -1;
    }
    toff = rd32(src + 0x1C);
    t1 = rd32(src + 0x34);
    t2 = rd32(src + 0x38);
    isz = rd32(src + 0x3C);
    if (toff + t1 > srclen || toff + t2 + isz > srclen) return -1;
    declen = lvl5_size(src + toff, t1);
    declen2 = lvl5_size(src + toff + t2, isz);
    if (declen == 0u || declen > 0x400000u) return -1;
    if (declen2 == 0u || declen2 > 0x400000u) return -1;
    tab = (uint8_t *)malloc(declen);
    img = (uint8_t *)malloc(declen2);
    if (tab == NULL || img == NULL) goto fail;
    got = lvl5_decomp(src + toff, t1, tab, declen);
    got2 = lvl5_decomp(src + toff + t2, isz, img, declen2);
    if (got <= 0 || got2 <= 0) goto fail;
    if (got >= 2 && (tab[0] | (tab[1] << 8)) == 0x453) {
        epos = 8u;
        elem = 4;
    }
    m = (bpp == 0) ? 32u : 64u * (uint32_t)bpp;
    count = ((uint32_t)got - epos) / (uint32_t)elem;
    if (count == 0u) goto fail;
    cols = ((uint32_t)w + 7u) / 8u;
    rows = ((uint32_t)h + 7u) / 8u;
    tiles = cols * rows;
    piclen = ((tiles > count) ? tiles : count) * m;
    pic = (uint8_t *)calloc(piclen, 1u);
    if (pic == NULL) goto fail;
    for (k = 0u; k < count; k++) {
        int32_t e;
        uint8_t *dst = pic + k * m;
        if (elem == 2) e = (int16_t)rd16(tab + epos + k * 2u);
        else e = (int32_t)rd32(tab + epos + k * 4u);
        if (e >= 0 && (uint32_t)e * m + m <= (uint32_t)got2)
            memcpy(dst, img + (uint32_t)e * m, m);
    }
    linear = tile_order_linear();
    {
        static int dbg = -1;
        if (dbg < 0) dbg = (getenv("HLE_IMGC_DEBUG") != NULL);
        if (dbg) {
            fprintf(stderr, "[imgc] fmt=%04X bits=%d bpt=%u toff=%u t1=%u "
                    "t2=%u isz=%u declen=%u declen2=%u got=%d got2=%d "
                    "elem=%d count=%u bpp=%d linear=%d pic=%02X%02X%02X%02X\n",
                    fmt, bits, bpt, toff, t1, t2, isz, declen, declen2, got,
                    got2, elem, count, bpp, linear, pic[0], pic[1], pic[2],
                    pic[3]);
        }
    }
    if (etc1) {
        uint32_t ty, tx, sy, sx;
        for (ty = 0u; ty < rows; ty++) {
            for (tx = 0u; tx < cols; tx++) {
                const uint8_t *tile = pic + (ty * cols + tx) * 32u;
                for (sy = 0u; sy < 2u; sy++)
                    for (sx = 0u; sx < 2u; sx++)
                        etc1_block(tile + (sy * 2u + sx) * 8u, rgb, w,
                                   (int)(tx * 8u + sx * 4u),
                                   (int)(ty * 8u + sy * 4u));
            }
        }
    } else if (linear) {
        blit_linear(pic, piclen, rgb, w, h, bpp);
    } else {
        blit_zorder(pic, piclen, rgb, w, h, bpp);
    }
    *out_w = w;
    *out_h = h;
    free(tab);
    free(img);
    free(pic);
    return 0;
fail:
    free(tab);
    free(img);
    free(pic);
    return -1;
}
