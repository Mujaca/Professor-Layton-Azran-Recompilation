/* Dekoder fuer Level-5 IMGC00 (.xi).
   Verfahren: 2 = Huffman4Bit, 3 = Huffman8Bit, 4 = RLE, 0 = roh.
   24-bpp-Bilder werden als RGB (BGR -> RGB) ausgegeben. */
#include "imgc.h"

#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
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

int imgc_decode(const uint8_t *src, uint32_t srclen,
                uint8_t *rgb, int *out_w, int *out_h) {
    uint32_t bpt, toff, t1, t2, isz, declen, declen2, epos = 0u, count, k;
    int bits, w, h, elem = 2, m = 0, got, got2, bpp;
    uint8_t *tab = NULL, *img = NULL, *pic = NULL;
    uint32_t piclen, w8, y, x, i = 0u;
    if (srclen < 0x48u || memcmp(src, "IMGC00", 6) != 0) return -1;
    bits = src[0x0D];
    w = (int)rd16(src + 0x10);
    h = (int)rd16(src + 0x12);
    bpt = rd16(src + 0x0E);
    if ((bits != 24 && bits != 32) || w <= 0 || h <= 0) return -1;
    bpp = bits / 8;
    if (bpt != (uint32_t)(64 * bpp)) return -1;
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
    count = ((uint32_t)got - epos) / (uint32_t)elem;
    if (count == 0u) goto fail;
    piclen = count * bpt;
    pic = (uint8_t *)malloc(piclen);
    if (pic == NULL) goto fail;
    for (k = 0u; k < count; k++) {
        int32_t e;
        uint8_t *dst = pic + k * bpt;
        if (elem == 2) e = (int16_t)rd16(tab + epos + k * 2u);
        else e = (int32_t)rd32(tab + epos + k * 4u);
        if (e < 0 || (uint32_t)e * bpt + bpt > (uint32_t)got2) {
            memset(dst, 0, bpt);
        } else {
            memcpy(dst, img + (uint32_t)e * bpt, bpt);
        }
    }
    w8 = ((uint32_t)w + 7u) & ~7u;
    for (y = 0u; y < (uint32_t)h; y++) {
        for (x = 0u; x < (uint32_t)w; x++) {
            int sx, sy;
            uint32_t o;
            master_get(y * w8 + x, w8, &sx, &sy);
            o = ((uint32_t)sy * w8 + (uint32_t)sx) * 3u;
            i = (y * (uint32_t)w + x) * 3u;
            if (bpp == 4) {
                o = ((uint32_t)sy * w8 + (uint32_t)sx) * 4u;
            }
            if (o + (uint32_t)bpp <= piclen) {
                if (bpp == 4) {
                    rgb[i + 0u] = pic[o + 3u];
                    rgb[i + 1u] = pic[o + 2u];
                    rgb[i + 2u] = pic[o + 1u];
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
