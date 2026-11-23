/* Test-Harness: dekodiert eine IMGC00-Datei mit runtime/imgc.c.
   Aufruf: imgc_dump <in.xi> <out.rgb>   -> stdout "W H" */
#include <stdio.h>
#include <stdlib.h>

#include "imgc.h"

int main(int argc, char **argv) {
    FILE *f;
    long n;
    unsigned char *in, *rgb;
    int w = 0, h = 0;
    if (argc < 3) {
        fprintf(stderr, "usage: %s in.xi out.rgb\n", argv[0]);
        return 2;
    }
    f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 2; }
    in = (unsigned char *)malloc((size_t)n);
    if (!in) { fclose(f); return 2; }
    if (fread(in, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(in); return 2; }
    fclose(f);
    /* Zielpuffer grosszuegig: Header sagt w/h, aber erst nach dem Decode
       bekannt -> Maximalgroesse aus dem Header lesen. */
    w = (int)((unsigned)in[0x10] | ((unsigned)in[0x11] << 8));
    h = (int)((unsigned)in[0x12] | ((unsigned)in[0x13] << 8));
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) { free(in); return 1; }
    rgb = (unsigned char *)calloc((size_t)w * (size_t)h * 3u, 1u);
    if (!rgb) { free(in); return 2; }
    if (imgc_decode(in, (uint32_t)n, rgb, &w, &h) != 0) {
        free(in);
        free(rgb);
        return 1;
    }
    f = fopen(argv[2], "wb");
    if (!f) { free(in); free(rgb); return 2; }
    fwrite(rgb, 1, (size_t)w * (size_t)h * 3u, f);
    fclose(f);
    free(in);
    free(rgb);
    printf("%d %d\n", w, h);
    return 0;
}
