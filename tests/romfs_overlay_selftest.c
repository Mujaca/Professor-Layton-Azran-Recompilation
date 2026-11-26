/* Selbsttest-Treiber fuer runtime/kernel/romfs_overlay.c (ohne Spieldaten).
 *
 *   romfs_overlay_selftest build <abbild> <l3_base> <mods> <ausgabe>
 *       baut das virtuelle Level-3 und schreibt es komplett nach <ausgabe>
 *       (Exit 0 = aktiv, 3 = inaktiv). Liest dabei zusaetzlich in
 *       ungeraden Stuecken, um die Extent-Suche ueber Grenzen zu pruefen.
 *   romfs_overlay_selftest decomp <eingabe> <ausgabe>
 *       Level-5-Dekompression einer Datei (Vergleich gegen Python).
 *
 * Gesteuert von tests/romfs_overlay_test.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "romfs_overlay.h"

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    long sz;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)sz + 1);
    *n = fread(b, 1, (size_t)sz, f);
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    if (argc == 4 && strcmp(argv[1], "decomp") == 0) {
        size_t n = 0, raw = 0;
        unsigned char *in = slurp(argv[2], &n), *out;
        FILE *f;
        if (in == NULL) return 2;
        out = rfo_l5_decompress(in, n, &raw);
        if (out == NULL) {
            printf("Dekompression fehlgeschlagen\n");
            return 1;
        }
        f = fopen(argv[3], "wb");
        fwrite(out, 1, raw, f);
        fclose(f);
        return 0;
    }
    if (argc == 6 && strcmp(argv[1], "build") == 0) {
        unsigned long long base = strtoull(argv[3], NULL, 0);
        unsigned long long size, off;
        unsigned char *all, *chunked;
        FILE *f;
        if (!rfo_init(argv[2], base, argv[4])) return 3;
        size = rfo_size();
        all = (unsigned char *)malloc((size_t)size + 64);
        chunked = (unsigned char *)malloc((size_t)size + 64);
        if (rfo_read(0, all, (size_t)size + 64) != size) {
            printf("Lesen am Stueck: falsche Laenge\n");
            return 1;
        }
        for (off = 0; off < size;) {
            size_t n = (size_t)(1 + (off * 7919) % 97);
            size_t got = rfo_read(off, chunked + off, n);
            if (got == 0) break;
            off += got;
        }
        if (off != size || memcmp(all, chunked, (size_t)size) != 0) {
            printf("Lesen in Stuecken weicht ab\n");
            return 1;
        }
        if (rfo_read(size, chunked, 16) != 0) {
            printf("Lesen hinter dem Ende liefert Daten\n");
            return 1;
        }
        f = fopen(argv[5], "wb");
        fwrite(all, 1, (size_t)size, f);
        fclose(f);
        rfo_shutdown();
        return 0;
    }
    fprintf(stderr, "Aufruf: build <abbild> <basis> <mods> <ausgabe> | decomp <ein> <aus>\n");
    return 2;
}
