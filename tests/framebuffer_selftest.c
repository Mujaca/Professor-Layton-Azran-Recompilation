/* Selbsttest der Bildausgabe: Spaltenlayout, Pixelformate, Stride,
 * Doppelpuffer/active_fb und VBlank-Latch mit synthetischen Framebuffern.
 * Kein Spielcode, keine Referenzbilder.
 *
 *   python tests/framebuffer_test.py
 *
 * Geprueft werden runtime/platform/framebuffer.h und
 * runtime/platform/gsp_fb.h gegen die in docs/REFERENCE.md belegten Regeln:
 *   Pixel (x,y) = address + x*stride + (rows-1-y)*bpp,
 *   FrameBufferInfo (active_fb, address_left/right, stride, format, shown_fb)
 *   wie Azahar video_core/gpu.cpp und renderer_software.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "framebuffer.h"
#include "gsp_fb.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define CHECK_PIX(buf, w, x, y, er, eg, eb)                                    \
    do {                                                                       \
        const uint8_t *p_ = (buf) + ((size_t)(y) * (size_t)(w) + (size_t)(x)) * 3u; \
        CHECK(p_[0] == (er) && p_[1] == (eg) && p_[2] == (eb),                 \
              "pixel (%d,%d) = %u,%u,%u erwartet %u,%u,%u",                   \
              (int)(x), (int)(y), p_[0], p_[1], p_[2], (er), (eg), (eb));      \
    } while (0)

static void put_rgb8(uint8_t *mem, uint32_t addr, uint32_t stride, int rows,
                     int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    uint8_t *p = mem + addr + (size_t)x * stride + (size_t)(rows - 1 - y) * 3u;
    p[0] = b; p[1] = g; p[2] = r;   /* RGB8 ist B,G,R gespeichert */
}

static void test_rgb8_column_layout(void) {
    /* 4 Spalten x 3 Zeilen, stride 0 -> rows*bpp. */
    static uint8_t mem[0x1000];
    uint8_t canvas[4 * 3 * 3];
    const uint32_t addr = 0x100u;
    FbScreen src;
    memset(mem, 0xFF, sizeof(mem));
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 3; y++) {
            put_rgb8(mem, addr, 9u, 3, x, y,
                     (uint8_t)(0x10 + x), (uint8_t)(0x20 + y), 0x7F);
        }
    }
    src.valid = 1; src.address = addr; src.stride = 0; src.format = 1;
    CHECK(fb_blit_screen(canvas, 4, 3, 0, 0, 4, 3, mem, sizeof(mem), &src) == 1,
          "RGB8-blit mit stride 0 abgelehnt");
    for (int x = 0; x < 4; x++) {
        for (int y = 0; y < 3; y++) {
            CHECK_PIX(canvas, 4, x, y, (uint8_t)(0x10 + x), (uint8_t)(0x20 + y), 0x7F);
        }
    }
    /* Zeilen sind innerhalb der Spalte umgekehrt: Offset 0 = unterste Zeile. */
    CHECK(mem[addr + 0] == 0x7F && mem[addr + 2] == (uint8_t)(0x10 + 0),
          "erste Spalte beginnt nicht mit der untersten Zeile");
    CHECK(mem[addr + 3 * 9 + 2] == (uint8_t)(0x10 + 3),
          "letzte Spalte liegt nicht bei x*stride");
}

static void test_stride(void) {
    /* stride 16 > rows*bpp (9): Fuellbytes zwischen den Spalten werden
     * ignoriert. */
    static uint8_t mem[0x1000];
    uint8_t canvas[5 * 3 * 3];
    const uint32_t addr = 0x40u;
    FbScreen src;
    memset(mem, 0xAA, sizeof(mem));
    for (int x = 0; x < 5; x++) {
        for (int y = 0; y < 3; y++) {
            put_rgb8(mem, addr, 16u, 3, x, y, (uint8_t)(x * 10), (uint8_t)(y * 20), 5);
        }
    }
    src.valid = 1; src.address = addr; src.stride = 16; src.format = 1;
    CHECK(fb_blit_screen(canvas, 5, 3, 0, 0, 5, 3, mem, sizeof(mem), &src) == 1,
          "RGB8-blit mit stride 16 abgelehnt");
    for (int x = 0; x < 5; x++) {
        for (int y = 0; y < 3; y++) {
            CHECK_PIX(canvas, 5, x, y, (uint8_t)(x * 10), (uint8_t)(y * 20), 5);
        }
    }
}

static void test_pixel_formats(void) {
    struct { uint32_t fmt; uint8_t bytes[4]; uint8_t r, g, b; } cases[] = {
        {0, {0x11, 0x22, 0x33, 0x44}, 0x44, 0x33, 0x22},              /* RGBA8 */
        {1, {0x22, 0x33, 0x44, 0x00}, 0x44, 0x33, 0x22},              /* RGB8 */
        {2, {0x00, 0xF8, 0x00, 0x00}, 0xFF, 0x00, 0x00},              /* RGB565 r=31 */
        {2, {0xE0, 0x07, 0x00, 0x00}, 0x00, 0xFF, 0x00},              /* RGB565 g=63 */
        {2, {0x1F, 0x00, 0x00, 0x00}, 0x00, 0x00, 0xFF},              /* RGB565 b=31 */
        {3, {0xFF, 0xFF, 0x00, 0x00}, 0xFF, 0xFF, 0xFF},              /* RGB5A1 */
        {3, {0x00, 0x00, 0x00, 0x00}, 0x00, 0x00, 0x00},              /* RGB5A1 0 */
        {4, {0x00, 0xF0, 0x00, 0x00}, 0xFF, 0x00, 0x00},              /* RGBA4 r=15 */
        {4, {0x00, 0x0F, 0x00, 0x00}, 0x00, 0xFF, 0x00},              /* RGBA4 g=15 */
        {4, {0xF0, 0x00, 0x00, 0x00}, 0x00, 0x00, 0xFF},              /* RGBA4 b=15 */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        FbPixel c;
        int ok = fb_decode_pixel(cases[i].fmt, cases[i].bytes, &c);
        CHECK(ok == 1, "Format %u nicht dekodiert", cases[i].fmt);
        CHECK(c.r == cases[i].r && c.g == cases[i].g && c.b == cases[i].b,
              "Format %u: %u,%u,%u erwartet %u,%u,%u", cases[i].fmt,
              c.r, c.g, c.b, cases[i].r, cases[i].g, cases[i].b);
    }
    CHECK(fb_bytes_per_pixel(0) == 4, "RGBA8 bpp");
    CHECK(fb_bytes_per_pixel(1) == 3, "RGB8 bpp");
    CHECK(fb_bytes_per_pixel(2) == 2 && fb_bytes_per_pixel(3) == 2 &&
          fb_bytes_per_pixel(4) == 2, "16-bit bpp");
    CHECK(fb_bytes_per_pixel(5) == 0 && fb_bytes_per_pixel(7) == 0,
          "reservierte Formate muessen bpp 0 haben");
    {
        uint8_t px[4] = {0, 0, 0, 0};
        FbPixel c;
        CHECK(fb_decode_pixel(6, px, &c) == 0, "Format 6 dekodiert");
    }
}

static void test_compose_both_screens(void) {
    /* Echte Screen-Groessen: oben 400x240 RGB8 (stride 720) rot, unten
     * 320x240 RGB8 blau. Beide Puffer liegen im selben Gast-Speicher. */
    const uint32_t top_addr = 0x1000u;
    const uint32_t bot_addr = 0x50000u;
    const size_t mem_size = 0x90000u;
    uint8_t *mem = (uint8_t *)calloc(1, mem_size);
    uint8_t *canvas = (uint8_t *)malloc((size_t)FB_CANVAS_W * FB_CANVAS_H * 3u);
    FbScreen top, bottom;
    if (!mem || !canvas) {
        CHECK(0, "Speicher fuer Compose-Test fehlt");
        free(mem); free(canvas);
        return;
    }
    for (int x = 0; x < FB_TOP_COLUMNS; x++)
        for (int y = 0; y < FB_TOP_ROWS; y++)
            put_rgb8(mem, top_addr, 720u, FB_TOP_ROWS, x, y, 0xC0, 0x10, 0x20);
    for (int x = 0; x < FB_BOTTOM_COLUMNS; x++)
        for (int y = 0; y < FB_BOTTOM_ROWS; y++)
            put_rgb8(mem, bot_addr, 720u, FB_BOTTOM_ROWS, x, y, 0x10, 0x20, 0xC0);
    top.valid = 1; top.address = top_addr; top.stride = 720; top.format = 1;
    bottom.valid = 1; bottom.address = bot_addr; bottom.stride = 720; bottom.format = 1;
    fb_compose(canvas, FB_CANVAS_W, FB_CANVAS_H, mem, mem_size, &top, &bottom);

    CHECK_PIX(canvas, FB_CANVAS_W, 0, 0, 0xC0, 0x10, 0x20);
    CHECK_PIX(canvas, FB_CANVAS_W, 399, 239, 0xC0, 0x10, 0x20);
    CHECK_PIX(canvas, FB_CANVAS_W, 0, 240, 0, 0, 0);          /* Luecke links */
    CHECK_PIX(canvas, FB_CANVAS_W, FB_BOTTOM_X, FB_BOTTOM_Y, 0x10, 0x20, 0xC0);
    CHECK_PIX(canvas, FB_CANVAS_W, FB_BOTTOM_X + 319, 479, 0x10, 0x20, 0xC0);
    CHECK_PIX(canvas, FB_CANVAS_W, FB_BOTTOM_X + 320, 479, 0, 0, 0); /* rechts */
    CHECK_PIX(canvas, FB_CANVAS_W, 200, 470, 0x10, 0x20, 0xC0);
    free(mem);
    free(canvas);
}

static void test_invalid_inputs(void) {
    static uint8_t mem[0x1000];
    uint8_t canvas[8 * 8 * 3];
    FbScreen src;
    memset(mem, 0, sizeof(mem));
    memset(canvas, 0x55, sizeof(canvas));

    src.valid = 0; src.address = 0x100; src.stride = 9; src.format = 1;
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "ungueltiger Screen wurde gezeichnet");
    src.valid = 1; src.address = 0x100;
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 1,
          "gueltiger Screen abgelehnt");
    src.address = 0;
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "Adresse 0 wurde gezeichnet");
    src.address = 0x100; src.format = 5;
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "reserviertes Format wurde gezeichnet");
    src.format = 1; src.stride = 8;   /* kleiner als rows*bpp (9) */
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "ueberlappender stride wurde gezeichnet");
    src.stride = 9; src.address = (uint32_t)(sizeof(mem) - 10);  /* 36 B noetig */
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "Puffer ausserhalb des Speichers wurde gezeichnet");
    src.address = 0x100;
    CHECK(fb_blit_screen(canvas, 4, 3, 1, 0, 4, 3, mem, sizeof(mem), &src) == 0,
          "Blit ausserhalb des Kanvas wurde gezeichnet");
    CHECK(fb_blit_screen(canvas, 8, 8, 0, 0, 4, 3, mem, sizeof(mem), NULL) == 0,
          "NULL-Screen wurde gezeichnet");
}

static void test_gsp_regs_and_latch(void) {
    GspFbRegs r;
    GspFbLatched latch;

    gsp_fb_regs_reset(&r, 0x1000u, 0x2000u, 720u, 1u);
    CHECK(r.active_fb == 0 && r.address_left1 == 0x1000u &&
          r.address_left2 == 0x2000u && r.stride == 720u,
          "Reset-Werte falsch");
    CHECK(gsp_fb_regs_visible_left(&r) == 0x1000u, "Reset zeigt nicht Puffer 1");
    CHECK(gsp_fb_regs_visible_right(&r) == 0x1000u, "Reset rechtes Auge");

    /* SetBufferSwap mit active_fb=1 fuellt Puffer 2; shown_fb=0 laesst die
     * Anzeige zunaechst auf Puffer 1 (active_fb bleibt 0). */
    gsp_fb_regs_apply(&r, 0, 1u, 0x3000u, 0u, 720u, 0x12340001u, 0u);
    CHECK(r.address_left2 == 0x3000u, "active_fb=1 schreibt nicht nach left2");
    CHECK(r.address_left1 == 0x1000u, "active_fb=1 veraendert left1");
    CHECK(r.format == 0x0001u, "Format nicht auf 16 Bit maskiert");
    CHECK(r.active_fb == 0u, "shown_fb=0 muss active_fb 0 lassen");
    CHECK(gsp_fb_regs_visible_left(&r) == 0x1000u, "sichtbarer Puffer falsch");

    /* VBlank-Latch: Register schon umgeschaltet, Anzeige noch alt. */
    gsp_fb_latch(&r, &latch);
    CHECK(latch.valid == 1 && latch.address == 0x1000u, "Latch vor Umschaltung");

    /* Jetzt shown_fb=1 -> active_fb=1, sichtbarer Puffer 2. */
    gsp_fb_regs_apply(&r, 0, 0u, 0x1001u, 0u, 720u, 1u, 1u);
    CHECK(r.active_fb == 1u, "shown_fb=1 setzt active_fb nicht");
    CHECK(gsp_fb_regs_visible_left(&r) == 0x3000u, "active_fb=1 zeigt nicht left2");
    gsp_fb_latch(&r, &latch);
    CHECK(latch.address == 0x3000u && latch.stride == 720u && latch.format == 1u,
          "VBlank-Latch uebernimmt neuen Puffer nicht");

    /* 3D oben: format 0x20/0x70 waehlt rechtes Auge, sonst zeigt es links. */
    gsp_fb_regs_apply(&r, 0, 1u, 0x4000u, 0x5000u, 720u, 0x0021u, 1u);
    CHECK(r.address_right2 == 0x5000u, "3D: address_right nicht uebernommen");
    CHECK(gsp_fb_regs_visible_right(&r) == 0x5000u, "3D: rechtes Auge falsch");
    gsp_fb_regs_apply(&r, 0, 1u, 0x4001u, 0x5001u, 720u, 0x0001u, 1u);
    CHECK(r.address_right2 == 0x4001u, "2D: rechtes Auge muss links spiegeln");

    /* screen 1 hat kein rechtes Auge: right bleibt unveraendert. */
    {
        GspFbRegs sub;
        gsp_fb_regs_reset(&sub, 0x6000u, 0x7000u, 720u, 1u);
        gsp_fb_regs_apply(&sub, 1, 0u, 0x8000u, 0x9000u, 720u, 1u, 0u);
        CHECK(sub.address_left1 == 0x8000u, "unten: linke Adresse fehlt");
        CHECK(sub.address_right1 == 0x6000u, "unten: rechtes Auge veraendert");
    }

    /* Adresse 0 bedeutet "nicht gesetzt": Register bleiben stehen. */
    gsp_fb_regs_apply(&r, 0, 0u, 0u, 0u, 0u, 0u, 1u);
    CHECK(gsp_fb_regs_visible_left(&r) == 0x4001u, "Adresse 0 hat Register geaendert");
}

static void test_bmp_writer(void) {
    const char *path = "tools/out/framebuffer_selftest.bmp";
    static uint8_t canvas[4 * 2 * 3];
    unsigned char hdr[54];
    unsigned char data[8 * 3];
    FILE *f;
    int w = 0, h = 0;
    short bpp = 0;
    /* Kanvas: obere Zeile (y=0) rot, untere (y=1) gruen. */
    memset(canvas, 0, sizeof(canvas));
    canvas[0] = 0xFF; canvas[1] = 0x00; canvas[2] = 0x00;
    canvas[4 * 3 + 0] = 0x00; canvas[4 * 3 + 1] = 0xFF; canvas[4 * 3 + 2] = 0x00;
    CHECK(fb_write_bmp(path, canvas, 4, 2) == 1, "BMP nicht geschrieben");
    f = fopen(path, "rb");
    CHECK(f != NULL, "BMP nicht lesbar");
    if (f == NULL) return;
    CHECK(fread(hdr, 1, 54, f) == 54, "BMP-Header kurz");
    memcpy(&w, hdr + 18, 4);
    memcpy(&h, hdr + 22, 4);
    memcpy(&bpp, hdr + 28, 2);
    CHECK(hdr[0] == 'B' && hdr[1] == 'M', "BMP-Magic fehlt");
    CHECK(w == 4 && h == 2, "BMP-Groesse %dx%d erwartet 4x2", w, h);
    CHECK(bpp == 24, "BMP bpp %d", (int)bpp);
    CHECK(fread(data, 1, 4 * 3, f) == 4 * 3, "BMP-Daten kurz");
    /* BMP ist bottom-up: erste Dateizeile ist die untere Kanvas-Zeile
     * (gruen) und in BGR gespeichert. */
    CHECK(data[0] == 0x00 && data[1] == 0xFF && data[2] == 0x00,
          "erste BMP-Zeile ist nicht die unterste Kanvas-Zeile");
    CHECK(fread(data, 1, 4 * 3, f) == 4 * 3, "BMP-Daten kurz");
    CHECK(data[0] == 0x00 && data[1] == 0x00 && data[2] == 0xFF,
          "zweite BMP-Zeile entspricht nicht Rot");
    fclose(f);
    remove(path);
}

int main(void) {
    printf("framebuffer_selftest: Spaltenlayout, Formate, GSP-FrameBufferInfo\n");
    test_rgb8_column_layout();
    test_stride();
    test_pixel_formats();
    test_compose_both_screens();
    test_invalid_inputs();
    test_gsp_regs_and_latch();
    test_bmp_writer();
    if (g_failures != 0) {
        printf("FEHLGESCHLAGEN: %d von %d Pruefungen\n", g_failures, g_checks);
        return 1;
    }
    printf("OK: %d Pruefungen\n", g_checks);
    return 0;
}
