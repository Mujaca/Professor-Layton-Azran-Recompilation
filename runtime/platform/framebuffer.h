/* Host-seitige Bildausgabe: GSP-Framebuffer als RGB24-Kanvas.
 *
 * Ein 3DS-Framebuffer ist spaltenweise gespeichert: Das erste Pixel einer
 * Spalte x liegt bei addr + x*stride, die Zeilen laufen innerhalb der Spalte
 * von unten (Offset 0) nach oben (Offset (rows-1)*bpp). Fuer einen Screen mit
 * `columns` Spalten und `rows` Zeilen gilt also
 *
 *   Pixel (x, y) = addr + x*stride + (rows-1-y)*bpp,  x in 0..columns-1.
 *
 * Quellen: docs/REFERENCE.md (GSP), Azahar
 * video_core/renderer_software/renderer_software.cpp (LoadFBToScreenInfo) und
 * video_core/pica/regs_external.h (FramebufferConfig: width=240, height=400
 * bzw. 320, stride=3*240, RGB8 als Startwerte).
 *
 * Decodiert werden alle fuenf GSP-Pixelformate; die Farbtiefe wird exakt wie
 * Azahar common/color.h aufgefuellt (5->8: v<<3|v>>2, 4->8: v<<4|v).
 *
 * Reine Funktionen ohne Runtime-Abhaengigkeiten; tests/framebuffer_selftest.c
 * prueft sie mit synthetischen Puffern.
 */
#ifndef FRAMEBUFFER_H
#define FRAMEBUFFER_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Bildschirmgroessen des 3DS (Spalten x Zeilen). */
#define FB_TOP_COLUMNS   400
#define FB_TOP_ROWS      240
#define FB_BOTTOM_COLUMNS 320
#define FB_BOTTOM_ROWS   240

#define FB_CANVAS_W FB_TOP_COLUMNS
#define FB_CANVAS_H (FB_TOP_ROWS + FB_BOTTOM_ROWS)
#define FB_BOTTOM_X ((FB_CANVAS_W - FB_BOTTOM_COLUMNS) / 2)
#define FB_BOTTOM_Y FB_TOP_ROWS

/* Was die Anzeige von einem Screen liest: der beim VBlank uebernommene
 * Registerstand (Adresse aus active_fb, Stride, Formatwort). */
typedef struct {
    int valid;          /* 0 = noch kein gueltiger Framebuffer */
    uint32_t address;   /* Gastadresse des sichtbaren Puffers */
    uint32_t stride;    /* Bytes zwischen zwei Spalten, 0 = rows*bpp */
    uint32_t format;    /* GSP-Formatwort, Bits 0..2 = Pixelformat */
} FbScreen;

typedef struct {
    uint8_t r, g, b;
} FbPixel;

static inline int fb_bytes_per_pixel(uint32_t format) {
    switch (format & 7u) {
        case 0: return 4;                 /* RGBA8  */
        case 1: return 3;                 /* RGB8   */
        case 2: case 3: case 4: return 2; /* RGB565, RGB5A1, RGBA4 */
        default: return 0;                /* reserviert */
    }
}

static inline uint8_t fb_5to8(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t fb_6to8(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t fb_4to8(uint32_t v) { return (uint8_t)((v << 4) | v); }

/* Dekodiert ein Pixel; liefert 0 bei unbekanntem Format. */
static inline int fb_decode_pixel(uint32_t format, const uint8_t *p, FbPixel *out) {
    uint32_t v;
    switch (format & 7u) {
        case 0: /* RGBA8: gespeichert A,B,G,R */
            out->r = p[3]; out->g = p[2]; out->b = p[1];
            return 1;
        case 1: /* RGB8: gespeichert B,G,R */
            out->r = p[2]; out->g = p[1]; out->b = p[0];
            return 1;
        case 2: /* RGB565 */
            memcpy(&v, p, 2);
            out->r = fb_5to8((v >> 11) & 0x1Fu);
            out->g = fb_6to8((v >> 5) & 0x3Fu);
            out->b = fb_5to8(v & 0x1Fu);
            return 1;
        case 3: /* RGB5A1 */
            memcpy(&v, p, 2);
            out->r = fb_5to8((v >> 11) & 0x1Fu);
            out->g = fb_5to8((v >> 6) & 0x1Fu);
            out->b = fb_5to8((v >> 1) & 0x1Fu);
            return 1;
        case 4: /* RGBA4 */
            memcpy(&v, p, 2);
            out->r = fb_4to8((v >> 12) & 0xFu);
            out->g = fb_4to8((v >> 8) & 0xFu);
            out->b = fb_4to8((v >> 4) & 0xFu);
            return 1;
        default:
            return 0;
    }
}

/* Prueft, ob ein Screen-Puffer vollstaendig im Gast-Speicher liegt. */
static inline int fb_screen_in_bounds(const FbScreen *src, int columns, int rows,
                                      size_t mem_size) {
    uint64_t bpp, stride, end;
    if (!src->valid || src->address == 0u) return 0;
    bpp = (uint64_t)fb_bytes_per_pixel(src->format);
    if (bpp == 0) return 0;
    stride = (src->stride != 0u) ? (uint64_t)src->stride : (uint64_t)rows * bpp;
    if (stride < (uint64_t)rows * bpp) return 0;   /* Spalten wuerden sich ueberlappen */
    end = (uint64_t)src->address + stride * (uint64_t)(columns - 1) +
          (uint64_t)rows * bpp;
    return end <= (uint64_t)mem_size;
}

/* Zeichnet einen Screen nach rgb (RGB24, canvas_w x canvas_h) an (dx, dy).
 * Liefert 1, wenn gezeichnet wurde. */
static inline int fb_blit_screen(uint8_t *rgb, int canvas_w, int canvas_h,
                                 int dx, int dy, int columns, int rows,
                                 const uint8_t *mem, size_t mem_size,
                                 const FbScreen *src) {
    int bpp, x, y;
    uint32_t stride;
    if (rgb == NULL || mem == NULL || src == NULL) return 0;
    if (columns <= 0 || rows <= 0) return 0;
    if (dx < 0 || dy < 0 || dx + columns > canvas_w || dy + rows > canvas_h) return 0;
    if (!fb_screen_in_bounds(src, columns, rows, mem_size)) return 0;
    bpp = fb_bytes_per_pixel(src->format);
    stride = (src->stride != 0u) ? src->stride : (uint32_t)(rows * bpp);
    for (x = 0; x < columns; x++) {
        const uint8_t *column = mem + (size_t)src->address + (size_t)x * stride;
        for (y = 0; y < rows; y++) {
            const uint8_t *p = column + (size_t)(rows - 1 - y) * (size_t)bpp;
            FbPixel c;
            uint8_t *o = rgb + ((size_t)(dy + y) * (size_t)canvas_w +
                                (size_t)(dx + x)) * 3u;
            if (!fb_decode_pixel(src->format, p, &c)) return 0;
            o[0] = c.r; o[1] = c.g; o[2] = c.b;
        }
    }
    return 1;
}

/* Setzt den Kanvas auf Schwarz und zeichnet beide Screens untereinander:
 * oben 400x240 linksbuendig, darunter der 320x240-Screen zentriert. */
static inline void fb_compose(uint8_t *rgb, int canvas_w, int canvas_h,
                              const uint8_t *mem, size_t mem_size,
                              const FbScreen *top, const FbScreen *bottom) {
    if (rgb == NULL) return;
    memset(rgb, 0, (size_t)canvas_w * (size_t)canvas_h * 3u);
    fb_blit_screen(rgb, canvas_w, canvas_h, 0, 0,
                   FB_TOP_COLUMNS, FB_TOP_ROWS, mem, mem_size, top);
    fb_blit_screen(rgb, canvas_w, canvas_h, FB_BOTTOM_X, FB_BOTTOM_Y,
                   FB_BOTTOM_COLUMNS, FB_BOTTOM_ROWS, mem, mem_size, bottom);
}

/* Schreibt den Kanvas als 24-Bit-BMP (bottom-up, BGR). Liefert 1 bei Erfolg. */
static inline int fb_write_bmp(const char *path, const uint8_t *rgb,
                               int w, int h) {
    FILE *f;
    int row, data_size, total, off = 54, ih = 40;
    short planes = 1, bpp = 24;
    unsigned char hdr[54];
    if (path == NULL || rgb == NULL || w <= 0 || h <= 0) return 0;
    f = fopen(path, "wb");
    if (f == NULL) return 0;
    row = w * 3;
    data_size = row * h;
    total = 54 + data_size;
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) {
        const uint8_t *src = rgb + (size_t)y * (size_t)w * 3u;
        for (int x = 0; x < w; x++) {
            unsigned char bgr[3] = {src[x * 3 + 2], src[x * 3 + 1], src[x * 3]};
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
    return 1;
}

#endif
