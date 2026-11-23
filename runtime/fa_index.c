/* ARC0-.fa-Index: Name -> Datei-Offset.
 *
 * Format wie tools/xfsatool/arc.cpp:
 *   header: magic, table1, table2, table3, filenameTable, dataOffset
 *   table3  : komprimierte FileEntryArc[] { crc32, unknown, offset, size }
 *   namens  : komprimierte NUL-getrennte Namen (Ordner enden auf '/')
 *
 * Damit loest der Port den Menue-Pfad (z. B. "seq/menu_logo.xq") gegen den
 * echten Archivindex auf, statt den Offset zu erzwingen.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fa_index.h"

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t crc32_l5(const unsigned char *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int k;
    for (i = 0; i < n; i++) {
        crc ^= p[i];
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static unsigned char *lzss_decode(const unsigned char *buf, size_t len,
                                 uint32_t raw_len, uint32_t *out_len)
{
    unsigned char *out = (unsigned char *)calloc(raw_len ? raw_len : 1, 1);
    size_t i = 4;
    uint32_t n = 0;
    unsigned flags = 0, mask = 0;
    if (out == NULL) return NULL;
    while (n < raw_len && i < len) {
        mask >>= 1;
        if (mask == 0) {
            flags = buf[i++];
            mask = 0x80u;
        }
        if (!(flags & mask)) {
            if (i >= len) break;
            out[n++] = buf[i++];
        } else {
            uint32_t pos, l;
            if (i + 1 >= len) break;
            pos = ((uint32_t)buf[i] << 8) | buf[i + 1];
            i += 2;
            l = (pos >> 12) + 2u + 1u;
            pos = (pos & 0xFFFu) + 1u;
            while (l-- && n < raw_len) {
                out[n] = out[n - pos];
                n++;
            }
        }
    }
    *out_len = n;
    return out;
}

static unsigned char *huf_decode(const unsigned char *buf, size_t len,
                                 uint32_t raw_len, int num_bits,
                                 uint32_t *out_len)
{
    unsigned char *out;
    const unsigned char *tree = buf + 4;
    size_t pak;
    uint32_t pos, nxt = 0, mask4 = 0, code = 0, n = 0, cur = 0;
    int nbits = 0;
    if (len < 8) return NULL;
    out = (unsigned char *)calloc(raw_len ? raw_len : 1, 1);
    if (out == NULL) return NULL;
    pak = 4u + ((uint32_t)buf[4] + 1u) * 2u;
    pos = tree[1];
    while (n < raw_len) {
        uint32_t ch;
        mask4 >>= 1;
        if (mask4 == 0) {
            if (pak + 4u > len) break;
            code = rd32(buf + pak);
            pak += 4u;
            mask4 = 0x80000000u;
        }
        nxt += ((pos & 0x3Fu) + 1u) * 2u;
        if (!(code & mask4)) {
            ch = pos & 0x80u;
            pos = tree[nxt];
        } else {
            ch = pos & 0x40u;
            pos = tree[nxt + 1u];
        }
        if (ch) {
            cur = ((cur << num_bits) | pos) & 0xFFu;
            nbits = (nbits + num_bits) & 7;
            if (nbits == 0) {
                out[n++] = (unsigned char)cur;
                cur = 0;
            }
            pos = tree[1];
            nxt = 0;
        }
    }
    *out_len = n;
    return out;
}

static unsigned char *decompress(const unsigned char *buf, size_t len,
                                 uint32_t *out_len)
{
    uint32_t size;
    if (len < 5) return NULL;
    size = rd32(buf) / 8u;
    if (len - 4u == size) {
        unsigned char *out = (unsigned char *)calloc(size ? size : 1, 1);
        if (out == NULL) return NULL;
        memcpy(out, buf + 4, size);
        *out_len = size;
        return out;
    }
    if (buf[4] == 0x0Fu || buf[4] == 0xFFu)
        return huf_decode(buf, len, size, buf[4] == 0x0Fu ? 4 : 8, out_len);
    return lzss_decode(buf, len, size, out_len);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

int fa_index_lookup(const char *archive_path, const char *name,
                    uint32_t *out_off, uint32_t *out_size)
{
    FILE *f;
    unsigned char *d = NULL, *names = NULL, *tab = NULL;
    long sz;
    uint32_t t3, names_off, data_off, nlen = 0, tlen = 0;
    const char *want_base;
    int found = 0;
    if (archive_path == NULL || name == NULL) return 0;
    f = fopen(archive_path, "rb");
    if (f == NULL) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz < 0x40) { fclose(f); return 0; }
    rewind(f);
    d = (unsigned char *)malloc((size_t)sz);
    if (d == NULL) { fclose(f); return 0; }
    if (fread(d, 1, (size_t)sz, f) != (size_t)sz) {
        free(d); fclose(f); return 0;
    }
    fclose(f);
    if (memcmp(d, "ARC0", 4) != 0) { free(d); return 0; }
    t3 = rd32(d + 0x0C);
    names_off = rd32(d + 0x10);
    data_off = rd32(d + 0x14);
    if (t3 >= names_off || names_off >= data_off || data_off > (uint32_t)sz) {
        free(d); return 0;
    }
    names = decompress(d + names_off, data_off - names_off, &nlen);
    tab = decompress(d + t3, names_off - t3, &tlen);
    if (names == NULL || tab == NULL) {
        free(names); free(tab); free(d); return 0;
    }
    want_base = base_name(name);
    {
        uint32_t o;
        size_t off_full = 0;
        while (off_full < nlen) {
            const char *full = (const char *)names + off_full;
            size_t flen = strlen(full);
            int match = (strcmp(full, name) == 0);
            if (!match && want_base != NULL) {
                const char *fb = base_name(full);
                match = (strcmp(fb, want_base) == 0);
            }
            if (match) {
                uint32_t crc = crc32_l5((const unsigned char *)base_name(full),
                                        strlen(base_name(full)));
                for (o = 0; o + 16u <= tlen; o += 16u) {
                    if (rd32(tab + o) == crc) {
                        if (out_off) *out_off = data_off + rd32(tab + o + 8u);
                        if (out_size) *out_size = rd32(tab + o + 12u);
                        found = 1;
                        break;
                    }
                }
            }
            if (found) break;
            off_full += flen + 1u;
        }
    }
    free(names);
    free(tab);
    free(d);
    return found;
}
