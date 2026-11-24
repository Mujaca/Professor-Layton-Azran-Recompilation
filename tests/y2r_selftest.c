/* Selbsttest Y2R-Konverter (runtime/platform/y2r.h).
 *
 * Synthetische Bilder, kein Spielcode. Geprueft werden:
 *  - YUV422/YUV420/YUYV-Eingabe gegen eine unabhaengige Referenzschleife
 *    (Pixel-Indizierung und Chroma-Subsampling),
 *  - alle vier Ausgabeformate inkl. Alpha (feste, von Hand gerechnete Bytes),
 *  - Rotation 90/180/270 je 8x8-Kachel,
 *  - lineare und PICA-8x8-Ausgabe (Morton-Order),
 *  - DMA-Einheiten (transfer_unit/gap) und das Fortschreiben der Puffer,
 *  - ungueltige Konfigurationen (Breite nicht durch 8, Block8x8 mit 7 Zeilen).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "y2r.h"

#define MEMSZ (1u << 22)
static uint8_t mem[MEMSZ];

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        ++failures;
        printf("FAIL %s\n", what);
    }
}

static Y2rMemory guest(void) {
    Y2rMemory m;
    m.mem = mem;
    m.size = MEMSZ;
    return m;
}

static uint8_t rd(uint32_t addr) {
    return mem[addr & 0x1FFFFFFFu];
}

static void wr(uint32_t addr, uint8_t v) {
    mem[addr & 0x1FFFFFFFu] = v;
}

/* Unabhaengige Referenz: ein Pixel aus vollen Ebenen dekodieren.
 *   YUV422: U/V bei (y*width + x)/2
 *   YUV420: U/V bei ((y/2)*width + x)/2
 *   YUYV  : ein Strom [Y0 U Y1 V ...]                   */
static void ref_pixel(int fmt, const uint8_t *pY, const uint8_t *pU,
                      const uint8_t *pV, unsigned width, unsigned height,
                      unsigned x, unsigned y,
                      const int16_t *c, int *r, int *g, int *b) {
    int Y, U, V;
    int cY, rr, gg, bb;
    (void)height;
    if (fmt == Y2R_IN_YUV422_INDIV8 || fmt == Y2R_IN_YUV422_INDIV16) {
        Y = pY[y * width + x];
        U = pU[(y * width + x) / 2];
        V = pV[(y * width + x) / 2];
    } else if (fmt == Y2R_IN_YUV420_INDIV8 || fmt == Y2R_IN_YUV420_INDIV16) {
        Y = pY[y * width + x];
        U = pU[((y / 2) * width + x) / 2];
        V = pV[((y / 2) * width + x) / 2];
    } else {
        Y = pY[(y * width + x) * 2];
        U = pY[(y * width + (x / 2) * 2) * 2 + 1];
        V = pY[(y * width + (x / 2) * 2) * 2 + 3];
    }
    cY = c[0] * Y;
    rr = cY + c[1] * V;
    gg = cY - c[2] * V - c[3] * U;
    bb = cY + c[4] * U;
    rr = ((rr >> 3) + c[5] + 0x18) >> 5;
    gg = ((gg >> 3) + c[6] + 0x18) >> 5;
    bb = ((bb >> 3) + c[7] + 0x18) >> 5;
    if (rr < 0) rr = 0; else if (rr > 255) rr = 255;
    if (gg < 0) gg = 0; else if (gg > 255) gg = 255;
    if (bb < 0) bb = 0; else if (bb > 255) bb = 255;
    *r = rr; *g = gg; *b = bb;
}

/* Morton-LUT (3dbrew/Azahar) fuer die PICA-8x8-Ausgabe. */
static const uint8_t morton[64] = {
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
};

/* Feste Testebene: Y = 16+14*x (mod 256), U/V mit Muster. */
#define IN_Y   0x00100000u
#define IN_U   0x00180000u
#define IN_V   0x001C0000u
#define OUT    0x00200000u

/* Index des Chroma-Bytes in der vollen Ebene (siehe ref_pixel). */
static uint32_t chroma_index(int fmt, unsigned width, unsigned x, unsigned y) {
    if (fmt == Y2R_IN_YUV420_INDIV8 || fmt == Y2R_IN_YUV420_INDIV16)
        return ((y / 2) * width + x) / 2;
    return (y * width + x) / 2;
}

static void make_planes(int fmt, unsigned width, unsigned height) {
    unsigned x, y;
    memset(mem, 0, MEMSZ);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            uint32_t ci = chroma_index(fmt, width, x, y);
            wr(IN_Y + y * width + x, (uint8_t)(16 + 14 * ((x + y) % 16)));
            if ((x & 1) == 0 &&
                (fmt == Y2R_IN_YUV422_INDIV8 || (y & 1) == 0)) {
                wr(IN_U + ci, (uint8_t)(64 + 8 * ((x / 2 + y / 2) % 8)));
                wr(IN_V + ci, (uint8_t)(32 + 11 * ((x / 2 + y) % 8)));
            }
        }
}

static Y2rConversion base_conversion(int fmt, int out_fmt, unsigned width,
                                     unsigned height, int rotation, int align) {
    Y2rConversion c;
    memset(&c, 0, sizeof(c));
    c.input_format = fmt;
    c.output_format = out_fmt;
    c.rotation = rotation;
    c.block_alignment = align;
    c.input_line_width = (uint16_t)width;
    c.input_lines = (uint16_t)height;
    memcpy(c.coefficients, y2r_standard_coefficients[Y2R_STD_ITU_REC601],
           sizeof(c.coefficients));
    c.alpha = 0x40;
    return c;
}

/* Bild aus dem Gast-Speicher lesen (lineare Ausgabe). */
static void get_rgb8(uint32_t base, unsigned width, unsigned x, unsigned y,
                     int *r, int *g, int *b) {
    uint32_t p = base + (y * width + x) * 3u;
    *b = rd(p);
    *g = rd(p + 1);
    *r = rd(p + 2);
}

/* Ein Testfall: Format gegen die Referenzschleife pruefen (linear, RGB8). */
static void test_format(int fmt, unsigned width, unsigned height,
                        const char *name) {
    Y2rMemory m = guest();
    Y2rConversion c = base_conversion(fmt, Y2R_OUT_RGB8, width, height, 0,
                                      Y2R_ALIGN_LINEAR);
    unsigned x, y;
    int rc;
    make_planes(fmt, width, height);
    if (fmt == Y2R_IN_YUYV422_INTERLEAVED) {
        /* YUYV-Strom bei IN_Y ablegen: [Y0 U Y1 V] je Pixelpaar. */
        unsigned i;
        int r, g, b;
        for (y = 0; y < height; ++y)
            for (x = 0; x < width; x += 2) {
                int Y0 = 16 + 14 * ((x + y) % 16);
                int Y1 = 16 + 14 * ((x + 1 + y) % 16);
                int U = 64 + 8 * ((x / 2 + y / 2) % 8);
                int V = 32 + 11 * ((x / 2 + y) % 8);
                uint32_t p = IN_Y + (y * width + x) * 2u;
                wr(p + 0, (uint8_t)Y0);
                wr(p + 1, (uint8_t)U);
                wr(p + 2, (uint8_t)Y1);
                wr(p + 3, (uint8_t)V);
                (void)r; (void)g; (void)b; (void)i;
            }
        c.src_yuyv.address = IN_Y;
        c.src_yuyv.image_size = width * height * 2u;
        c.src_yuyv.transfer_unit = (uint16_t)(width * 2u);
        c.src_yuyv.gap = 0;
    } else if (fmt == Y2R_IN_YUV420_INDIV8) {
        c.src_y.address = IN_Y;
        c.src_y.image_size = width * height;
        c.src_y.transfer_unit = (uint16_t)width;
        c.src_u.address = IN_U;
        c.src_u.image_size = width * height / 4u;
        c.src_u.transfer_unit = (uint16_t)(width / 2u);
        c.src_v.address = IN_V;
        c.src_v.image_size = width * height / 4u;
        c.src_v.transfer_unit = (uint16_t)(width / 2u);
    } else {
        c.src_y.address = IN_Y;
        c.src_y.image_size = width * height;
        c.src_y.transfer_unit = (uint16_t)width;
        c.src_u.address = IN_U;
        c.src_u.image_size = width * height / 2u;
        c.src_u.transfer_unit = (uint16_t)(width / 2u);
        c.src_v.address = IN_V;
        c.src_v.image_size = width * height / 2u;
        c.src_v.transfer_unit = (uint16_t)(width / 2u);
    }
    c.dst.address = OUT;
    c.dst.image_size = width * height * 3u;
    c.dst.transfer_unit = (uint16_t)(width * 3u);
    c.dst.gap = 0;

    rc = y2r_perform_conversion(&m, &c);
    check(rc == 0, name);
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            int er, eg, eb, ar, ag, ab;
            ref_pixel(fmt, mem + IN_Y, mem + IN_U, mem + IN_V, width, height, x, y,
                      c.coefficients,
                      &er, &eg, &eb);
            get_rgb8(OUT, width, x, y, &ar, &ag, &ab);
            if (!(er == ar && eg == ag && eb == ab)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "%s Pixel (%u,%u): %d/%d/%d erwartet %d/%d/%d",
                         name, x, y, ar, ag, ab, er, eg, eb);
                check(0, msg);
                return;
            }
        }
    }
    check(c.dst.address == OUT + width * height * 3u, "dst.address fortgeschrieben");
    check(c.dst.image_size == 0, "dst.image_size verbraucht");
    printf("OK %s (%ux%u)\n", name, width, height);
}

#define IN16_Y 0x00300000u
#define IN16_U 0x00380000u
#define IN16_V 0x003C0000u

/* Kompakte Ebenen auf 16 Bit erweitern: LSB = Wert, MSB = 0xAA (muss
 * ignoriert werden; Azahar ReceiveData<2> liest nur jedes zweite Byte). */
static void expand16(int fmt, unsigned width, unsigned height) {
    unsigned x, y;
    memset(mem + IN16_Y, 0xAA, width * height * 2u);
    memset(mem + IN16_U, 0xAA, width * height / 2u * 2u);
    memset(mem + IN16_V, 0xAA, width * height / 2u * 2u);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            uint32_t o = (y * width + x) * 2u;
            uint32_t ci = chroma_index(fmt, width, x, y);
            wr(IN16_Y + o, rd(IN_Y + y * width + x));
            if ((x & 1) == 0 && (fmt == Y2R_IN_YUV422_INDIV16 || (y & 1) == 0)) {
                wr(IN16_U + ci * 2u, rd(IN_U + ci));
                wr(IN16_V + ci * 2u, rd(IN_V + ci));
            }
        }
}

/* 16-Bit-Eingabeformate gegen dieselbe Referenz (nur LSB zaehlt). */
static void test_format16(int fmt, unsigned width, unsigned height,
                          const char *name) {
    Y2rMemory m = guest();
    Y2rConversion c = base_conversion(fmt, Y2R_OUT_RGB8, width, height, 0,
                                      Y2R_ALIGN_LINEAR);
    unsigned x, y;
    make_planes(fmt, width, height);
    expand16(fmt, width, height);
    c.src_y.address = IN16_Y;
    c.src_y.image_size = width * height * 2u;
    c.src_y.transfer_unit = (uint16_t)(width * 2u);
    c.src_u.address = IN16_U;
    c.src_v.address = IN16_V;
    if (fmt == Y2R_IN_YUV420_INDIV16) {
        c.src_u.image_size = width * height / 2u;
        c.src_v.image_size = width * height / 2u;
        c.src_u.transfer_unit = (uint16_t)(width * 2u);
        c.src_v.transfer_unit = (uint16_t)(width * 2u);
    } else {
        c.src_u.image_size = width * height;
        c.src_v.image_size = width * height;
        c.src_u.transfer_unit = (uint16_t)width;
        c.src_v.transfer_unit = (uint16_t)width;
    }
    c.dst.address = OUT;
    c.dst.image_size = width * height * 3u;
    c.dst.transfer_unit = (uint16_t)(width * 3u);
    check(y2r_perform_conversion(&m, &c) == 0, name);
    for (y = 0; y < height; ++y)
        for (x = 0; x < width; ++x) {
            int er, eg, eb, ar, ag, ab;
            ref_pixel(fmt, mem + IN_Y, mem + IN_U, mem + IN_V, width, height,
                      x, y, c.coefficients, &er, &eg, &eb);
            get_rgb8(OUT, width, x, y, &ar, &ag, &ab);
            if (!(er == ar && eg == ag && eb == ab)) {
                char msg[160];
                snprintf(msg, sizeof(msg), "%s Pixel (%u,%u): %d/%d/%d erwartet %d/%d/%d",
                         name, x, y, ar, ag, ab, er, eg, eb);
                check(0, msg);
                return;
            }
        }
    printf("OK %s (%ux%u)\n", name, width, height);
}

/* Feste Bytes fuer das Einheitsbild Y=100, U=V=128 (Rec601). */
static void test_output_formats(void) {
    static const struct { int fmt; uint8_t b0, b1, b2, b3; int n; } cases[] = {
        {Y2R_OUT_RGB8,   100, 101, 100, 0,   3},
        {Y2R_OUT_RGBA8,   64, 100, 101, 100, 4}, /* A,B,G,R */
        {Y2R_OUT_RGB565, 0x2C, 0x63, 0, 0,  2},
        {Y2R_OUT_RGB5A1, 0x18, 0x63, 0, 0,  2},
    };
    unsigned k;
    for (k = 0; k < sizeof(cases) / sizeof(cases[0]); ++k) {
        Y2rMemory m = guest();
        Y2rConversion c = base_conversion(Y2R_IN_YUV422_INDIV8, cases[k].fmt,
                                          8, 8, 0, Y2R_ALIGN_LINEAR);
        unsigned i;
        int rc;
        memset(mem, 0, MEMSZ);
        for (i = 0; i < 64; ++i) {
            wr(IN_Y + i, 100);
            if (i < 32) {
                wr(IN_U + i, 128);
                wr(IN_V + i, 128);
            }
        }
        c.src_y.address = IN_Y; c.src_y.image_size = 64;
        c.src_y.transfer_unit = 8;
        c.src_u.address = IN_U; c.src_u.image_size = 32;
        c.src_u.transfer_unit = 4;
        c.src_v.address = IN_V; c.src_v.image_size = 32;
        c.src_v.transfer_unit = 4;
        c.dst.address = OUT;
        c.dst.image_size = 64 * (uint32_t)cases[k].n;
        c.dst.transfer_unit = (uint16_t)(8 * cases[k].n);
        rc = y2r_perform_conversion(&m, &c);
        check(rc == 0, "Ausgabeformat konvertiert");
        check(rd(OUT + 0) == cases[k].b0, "Ausgabeformat Byte 0");
        check(rd(OUT + 1) == cases[k].b1, "Ausgabeformat Byte 1");
        if (cases[k].n > 2) check(rd(OUT + 2) == cases[k].b2, "Ausgabeformat Byte 2");
        if (cases[k].n > 3) check(rd(OUT + 3) == cases[k].b3, "Ausgabeformat Byte 3");
        /* letztes Pixel */
        check(rd(OUT + 63u * (unsigned)cases[k].n) == cases[k].b0,
              "Ausgabeformat letztes Pixel");
    }
    printf("OK Ausgabeformate (RGB8/RGBA8/RGB565/RGB5A1)\n");
}

/* Rotation je 8x8-Kachel: 8x8-Eingang, linear, RGB8. */
static void test_rotation(void) {
    static const int rots[3] = {Y2R_ROT_CW90, Y2R_ROT_CW180, Y2R_ROT_CW270};
    unsigned k;
    for (k = 0; k < 3; ++k) {
        Y2rMemory m = guest();
        Y2rConversion c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8,
                                          8, 8, rots[k], Y2R_ALIGN_LINEAR);
        int rc, x, y;
        make_planes(Y2R_IN_YUV422_INDIV8, 8, 8);
        c.src_y.address = IN_Y; c.src_y.image_size = 64; c.src_y.transfer_unit = 8;
        c.src_u.address = IN_U; c.src_u.image_size = 32; c.src_u.transfer_unit = 4;
        c.src_v.address = IN_V; c.src_v.image_size = 32; c.src_v.transfer_unit = 4;
        c.dst.address = 0; c.dst.image_size = 64 * 3; c.dst.transfer_unit = 24;
        c.dst.address = OUT;
        rc = y2r_perform_conversion(&m, &c);
        check(rc == 0, "Rotation konvertiert");
        for (y = 0; y < 8; ++y) {
            for (x = 0; x < 8; ++x) {
                int sx, sy, er, eg, eb, ar, ag, ab;
                if (rots[k] == Y2R_ROT_CW90) {
                    sx = y; sy = 7 - x;
                } else if (rots[k] == Y2R_ROT_CW180) {
                    sx = 7 - x; sy = 7 - y;
                } else {
                    sx = 7 - y; sy = x;
                }
                ref_pixel(Y2R_IN_YUV422_INDIV8, mem + IN_Y, mem + IN_U, mem + IN_V,
                          8, 8, (unsigned)sx, (unsigned)sy, c.coefficients,
                          &er, &eg, &eb);
                get_rgb8(OUT, 8, (unsigned)x, (unsigned)y, &ar, &ag, &ab);
                if (!(er == ar && eg == ag && eb == ab)) {
                    char msg[128];
                    snprintf(msg, sizeof(msg), "Rotation %d Pixel (%d,%d)",
                             rots[k], x, y);
                    check(0, msg);
                    return;
                }
            }
        }
    }
    printf("OK Rotationen 90/180/270\n");
    /* 16x8, 180 Grad: Kachelreihenfolge invertiert = ganzes Bild gedreht. */
    {
        Y2rMemory m = guest();
        Y2rConversion c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8,
                                          16, 8, Y2R_ROT_CW180, Y2R_ALIGN_LINEAR);
        int rc, x, y;
        make_planes(Y2R_IN_YUV422_INDIV8, 16, 8);
        c.src_y.address = IN_Y; c.src_y.image_size = 128; c.src_y.transfer_unit = 16;
        c.src_u.address = IN_U; c.src_u.image_size = 64; c.src_u.transfer_unit = 8;
        c.src_v.address = IN_V; c.src_v.image_size = 64; c.src_v.transfer_unit = 8;
        c.dst.address = OUT; c.dst.image_size = 128 * 3; c.dst.transfer_unit = 48;
        rc = y2r_perform_conversion(&m, &c);
        check(rc == 0, "Rotation 180 (16x8)");
        for (y = 0; y < 8; ++y)
            for (x = 0; x < 16; ++x) {
                int er, eg, eb, ar, ag, ab;
                ref_pixel(Y2R_IN_YUV422_INDIV8, mem + IN_Y, mem + IN_U, mem + IN_V,
                          16, 8, (unsigned)(15 - x), (unsigned)(7 - y), c.coefficients,
                          &er, &eg, &eb);
                get_rgb8(OUT, 16, (unsigned)x, (unsigned)y, &ar, &ag, &ab);
                check(er == ar && eg == ag && eb == ab, "Rotation 180 (16x8) Pixel");
            }
    }
}

/* Block8x8: ent-swizzeltes Ergebnis muss dem linearen entsprechen. */
static void test_block8x8(void) {
    Y2rMemory m = guest();
    Y2rConversion c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8,
                                      16, 8, Y2R_ROT_NONE, Y2R_ALIGN_BLOCK8X8);
    int rc, x, y, tile;
    make_planes(Y2R_IN_YUV422_INDIV8, 16, 8);
    c.src_y.address = IN_Y; c.src_y.image_size = 128; c.src_y.transfer_unit = 16;
    c.src_u.address = IN_U; c.src_u.image_size = 64; c.src_u.transfer_unit = 8;
    c.src_v.address = IN_V; c.src_v.image_size = 64; c.src_v.transfer_unit = 8;
    c.dst.address = OUT; c.dst.image_size = 128 * 3; c.dst.transfer_unit = 48;
    rc = y2r_perform_conversion(&m, &c);
    check(rc == 0, "Block8x8 konvertiert");
    for (tile = 0; tile < 2; ++tile)
        for (y = 0; y < 8; ++y)
            for (x = 0; x < 8; ++x) {
                int er, eg, eb, ar, ag, ab;
                uint32_t pos = (uint32_t)(tile * 64 + morton[y * 8 + x]) * 3u;
                er = eg = eb = 0;
                ref_pixel(Y2R_IN_YUV422_INDIV8, mem + IN_Y, mem + IN_U, mem + IN_V,
                          16, 8, (unsigned)(tile * 8 + x), (unsigned)y, c.coefficients,
                          &er, &eg, &eb);
                ar = rd(OUT + pos + 2);
                ag = rd(OUT + pos + 1);
                ab = rd(OUT + pos);
                check(er == ar && eg == ag && eb == ab, "Block8x8 Pixel");
            }
    printf("OK Block8x8 (Morton-Order)\n");
}

/* DMA: gap zwischen den Transferunits, Puffer-Fortschritt, ungueltige Werte. */
static void test_dma_and_invalid(void) {
    Y2rMemory m = guest();
    Y2rConversion c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8,
                                      8, 8, Y2R_ROT_NONE, Y2R_ALIGN_LINEAR);
    int rc;
    unsigned i;
    make_planes(Y2R_IN_YUV422_INDIV8, 8, 8);
    c.src_y.address = IN_Y; c.src_y.image_size = 64; c.src_y.transfer_unit = 8;
    c.src_u.address = IN_U; c.src_u.image_size = 32; c.src_u.transfer_unit = 4;
    c.src_v.address = IN_V; c.src_v.image_size = 32; c.src_v.transfer_unit = 4;
    c.dst.address = OUT;
    c.dst.image_size = 8 * (24u + 8u);
    c.dst.transfer_unit = 24;
    c.dst.gap = 8;
    rc = y2r_perform_conversion(&m, &c);
    check(rc == 0, "DMA mit gap");
    check(c.dst.address == OUT + 8u * 32u, "DMA dst.address mit gap");
    /* Pixel liegen bei 32-Byte-Zeilenabstand. */
    for (i = 0; i < 8; ++i) {
        int er, eg, eb, ar, ag, ab;
        ref_pixel(Y2R_IN_YUV422_INDIV8, mem + IN_Y, mem + IN_U, mem + IN_V,
                  8, 8, i, 3, c.coefficients,
                  &er, &eg, &eb);
        ar = rd(OUT + 3u * 32u + i * 3u + 2);
        ag = rd(OUT + 3u * 32u + i * 3u + 1);
        ab = rd(OUT + 3u * 32u + i * 3u);
        check(er == ar && eg == ag && eb == ab, "DMA gap Pixel");
    }

    /* Breite nicht durch 8 teilbar -> -1 (kein Completion-Interrupt). */
    c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8, 12, 8, 0,
                        Y2R_ALIGN_LINEAR);
    check(y2r_perform_conversion(&m, &c) < 0, "ungueltige Breite");
    /* Block8x8 mit 7 Zeilen -> -1. */
    c = base_conversion(Y2R_IN_YUV422_INDIV8, Y2R_OUT_RGB8, 8, 7, 0,
                        Y2R_ALIGN_BLOCK8X8);
    check(y2r_perform_conversion(&m, &c) < 0, "Block8x8 mit 7 Zeilen");
    printf("OK DMA-Einheiten und ungueltige Konfigurationen\n");
}

int main(void) {
    test_format(Y2R_IN_YUV422_INDIV8, 16, 8, "YUV422_Indiv8");
    test_format(Y2R_IN_YUV420_INDIV8, 16, 16, "YUV420_Indiv8");
    test_format(Y2R_IN_YUYV422_INTERLEAVED, 8, 8, "YUYV422_Interleaved");
    test_format16(Y2R_IN_YUV422_INDIV16, 16, 8, "YUV422_Indiv16");
    test_format16(Y2R_IN_YUV420_INDIV16, 16, 16, "YUV420_Indiv16");
    test_output_formats();
    test_rotation();
    test_block8x8();
    test_dma_and_invalid();
    if (failures) {
        printf("%d Test(s) fehlgeschlagen\n", failures);
        return 1;
    }
    printf("OK Y2R-Konverter\n");
    return 0;
}
