/* Y2R (YUV->RGB-Konverter) – reine Umsetzung von Azahar src/core/hw/y2r.cpp
 * und src/core/hle/service/cam/y2r_u.{h,cpp}, ohne Emulator-Abhaengigkeiten.
 *
 * Das Y2R arbeitet das Bild in 8 Zeilen hohen Streifen ab:
 *  1. CDMA-Eingabe: je Streifen `row_height * width` Pixel aus Y/U/V bzw.
 *     YUYV lesen. `transfer_unit`/`gap` beschreiben die DMA-Einheiten; nach
 *     jeder Einheit werden `gap` Bytes uebersprungen (ReceiveData<N>).
 *  2. YUV->RGB mit 2.8-Festkomma-Koeffizienten (c0..c4) und 11.5-Offsets
 *     (c5..c7):
 *       R = (c0*Y + c1*V) >> 3 + c5 + 0x18
 *       G = (c0*Y - c2*V - c3*U) >> 3 + c6 + 0x18
 *       B = (c0*Y + c4*U) >> 3 + c7 + 0x18
 *     anschliessend >> 5 und auf 0..255 begrenzen (bit-exakt zur Hardware
 *     nach Azahar-Kommentar).
 *  3. Optionales Rotieren jedes 8x8-Kachels um 90/180/270 Grad.
 *  4. Ausgabe linear (Zeilen) oder als PICA-8x8-Kacheln (Morton-Order).
 *  5. CDMA-Ausgabe im Zielformat RGBA8/RGB8/RGB5A1/RGB565 mit Alpha.
 *
 * Das Spiel (Azahar-Referenzspur) nutzt z. B. YUV420_Indiv8, 400x240,
 * Eingabe-Stride 400/200 (U/V), Ausgabe RGB8 mit 9600-Byte-Transferunits,
 * Block8x8, ITU_Rec601_Scaling und Alpha 255 (Video-Wiedergabe).
 *
 * Die Puffer werden wie in Azahar fortgeschrieben (`address`/`image_size`).
 * Zugriffe ausserhalb des Gast-Speichers lesen 0 bzw. werden verworfen (wie
 * arm_mem_read/arm_mem_write in der uebrigen Runtime).
 */
#ifndef Y2R_H
#define Y2R_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
    Y2R_IN_YUV422_INDIV8 = 0,
    Y2R_IN_YUV420_INDIV8 = 1,
    Y2R_IN_YUV422_INDIV16 = 2,
    Y2R_IN_YUV420_INDIV16 = 3,
    Y2R_IN_YUYV422_INTERLEAVED = 4,
};

enum {
    Y2R_OUT_RGBA8 = 0,
    Y2R_OUT_RGB8 = 1,
    Y2R_OUT_RGB5A1 = 2,
    Y2R_OUT_RGB565 = 3,
};

enum {
    Y2R_ROT_NONE = 0,
    Y2R_ROT_CW90 = 1,
    Y2R_ROT_CW180 = 2,
    Y2R_ROT_CW270 = 3,
};

enum {
    Y2R_ALIGN_LINEAR = 0,
    Y2R_ALIGN_BLOCK8X8 = 1,
};

enum {
    Y2R_STD_ITU_REC601 = 0,
    Y2R_STD_ITU_REC709 = 1,
    Y2R_STD_ITU_REC601_SCALING = 2,
    Y2R_STD_ITU_REC709_SCALING = 3,
    Y2R_STD_COUNT = 4,
};

#define Y2R_MAX_LINE_WIDTH 1024u
#define Y2R_MAX_TILES      (Y2R_MAX_LINE_WIDTH / 8u)
#define Y2R_TILE_SIZE      64u

/* Azahar ConversionBuffer: address, image_size, transfer_unit, gap. */
typedef struct {
    uint32_t address;
    uint32_t image_size;
    uint16_t transfer_unit;
    uint16_t gap;
} Y2rBuffer;

/* Azahar ConversionConfiguration (Werte wie ueber IPC gesetzt). */
typedef struct {
    int input_format;
    int output_format;
    int rotation;
    int block_alignment;
    uint16_t input_line_width;
    uint16_t input_lines;
    int16_t coefficients[8];
    uint8_t padding;
    uint16_t alpha;
    Y2rBuffer src_y, src_u, src_v, src_yuyv, dst;
} Y2rConversion;

/* Azahar ConversionParameters (12 Byte, IPC-Paket). */
typedef struct {
    uint8_t input_format;
    uint8_t output_format;
    uint8_t rotation;
    uint8_t block_alignment;
    uint16_t input_line_width;
    uint16_t input_lines;
    uint8_t standard_coefficient;
    uint8_t padding;
    uint16_t alpha;
} Y2rParameters;

/* Standardkoeffizienten (Azahar y2r_u.cpp standard_coefficients). */
static const int16_t y2r_standard_coefficients[Y2R_STD_COUNT][8] = {
    {0x100, 0x166, 0xB6, 0x58, 0x1C5, -0x166F, 0x10EE, -0x1C5B}, /* ITU_Rec601 */
    {0x100, 0x193, 0x77, 0x2F, 0x1DB, -0x1933, 0xA7C, -0x1D51},  /* ITU_Rec709 */
    {0x12A, 0x198, 0xD0, 0x64, 0x204, -0x1BDE, 0x10F2, -0x229B}, /* ITU_Rec601_Scaling */
    {0x12A, 0x1CA, 0x88, 0x36, 0x21C, -0x1F04, 0x99C, -0x2421},  /* ITU_Rec709_Scaling */
};

typedef struct {
    uint8_t *mem;
    uint32_t size;
} Y2rMemory;

/* ---- Gast-Speicher (0x1FFFFFFF-Maske wie der Rest der Runtime) ---- */
static inline uint8_t y2r_rd8(const Y2rMemory *m, uint32_t addr) {
    addr &= 0x1FFFFFFFu;
    return addr < m->size ? m->mem[addr] : 0u;
}

static inline void y2r_wr8(const Y2rMemory *m, uint32_t addr, uint8_t v) {
    addr &= 0x1FFFFFFFu;
    if (addr < m->size) m->mem[addr] = v;
}

/* ReceiveData<N>: liest `amount` Pixel, wobei je `transfer_unit` Bytes
 * `gap` Bytes uebersprungen werden; bei N==2 wird nur das LSB genutzt. */
static void y2r_receive_data(const Y2rMemory *m, uint8_t *out, Y2rBuffer *buf,
                             uint32_t amount, int n) {
    uint32_t unit = (uint32_t)(buf->transfer_unit / (uint16_t)n);
    uint32_t address = buf->address;
    if (unit == 0) return;
    while (amount > 0) {
        uint32_t chunk = amount < unit ? amount : unit;
        uint32_t i;
        for (i = 0; i < chunk; ++i)
            out[i] = y2r_rd8(m, address + i * (uint32_t)n);
        out += chunk;
        address += (uint32_t)buf->transfer_unit + buf->gap;
        buf->image_size -= buf->transfer_unit;
        amount -= chunk;
    }
    buf->address = address;
}

/* Ein 8-Zeilen-Streifen wird dekodiert und kachelweise als RGB32 abgelegt. */
static void y2r_convert_strip(const uint8_t *in_y, const uint8_t *in_u,
                              const uint8_t *in_v, uint32_t *tiles,
                              int format, unsigned width, unsigned height,
                              const int16_t *c) {
    unsigned x, y;
    for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
            int32_t Y, U, V;
            int32_t cY, r, g, b;
            uint32_t *out;
            if (format == Y2R_IN_YUV422_INDIV8 || format == Y2R_IN_YUV422_INDIV16) {
                Y = in_y[y * width + x];
                U = in_u[(y * width + x) / 2];
                V = in_v[(y * width + x) / 2];
            } else if (format == Y2R_IN_YUV420_INDIV8 || format == Y2R_IN_YUV420_INDIV16) {
                Y = in_y[y * width + x];
                U = in_u[((y / 2) * width + x) / 2];
                V = in_v[((y / 2) * width + x) / 2];
            } else { /* Y2R_IN_YUYV422_INTERLEAVED */
                Y = in_y[(y * width + x) * 2];
                U = in_y[(y * width + (x / 2) * 2) * 2 + 1];
                V = in_y[(y * width + (x / 2) * 2) * 2 + 3];
            }

            cY = c[0] * Y;
            r = cY + c[1] * V;
            g = cY - c[2] * V - c[3] * U;
            b = cY + c[4] * U;
            r = (r >> 3) + c[5] + 0x18;
            g = (g >> 3) + c[6] + 0x18;
            b = (b >> 3) + c[7] + 0x18;
            r >>= 5;
            g >>= 5;
            b >>= 5;
            if (r < 0) r = 0; else if (r > 0xFF) r = 0xFF;
            if (g < 0) g = 0; else if (g > 0xFF) g = 0xFF;
            if (b < 0) b = 0; else if (b > 0xFF) b = 0xFF;

            /* Kachel x/8, innerhalb der Kachel Zeile y, Spalte x%8. */
            out = &tiles[(x / 8u) * Y2R_TILE_SIZE + y * 8u + (x % 8u)];
            *out = ((uint32_t)r << 24) | ((uint32_t)g << 16) | ((uint32_t)b << 8);
        }
    }
}

static const uint8_t y2r_linear_lut[Y2R_TILE_SIZE] = {
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, 14, 15,
    16, 17, 18, 19, 20, 21, 22, 23,
    24, 25, 26, 27, 28, 29, 30, 31,
    32, 33, 34, 35, 36, 37, 38, 39,
    40, 41, 42, 43, 44, 45, 46, 47,
    48, 49, 50, 51, 52, 53, 54, 55,
    56, 57, 58, 59, 60, 61, 62, 63,
};

/* PICA-8x8-Kachelordnung (Morton) fuer BlockAlignment::Block8x8. */
static const uint8_t y2r_morton_lut[Y2R_TILE_SIZE] = {
     0,  1,  4,  5, 16, 17, 20, 21,
     2,  3,  6,  7, 18, 19, 22, 23,
     8,  9, 12, 13, 24, 25, 28, 29,
    10, 11, 14, 15, 26, 27, 30, 31,
    32, 33, 36, 37, 48, 49, 52, 53,
    34, 35, 38, 39, 50, 51, 54, 55,
    40, 41, 44, 45, 56, 57, 60, 61,
    42, 43, 46, 47, 58, 59, 62, 63,
};

static void y2r_rotate_tile0(const uint32_t *in, uint32_t *out, int height,
                             const uint8_t *map) {
    int i;
    for (i = 0; i < height * 8; ++i) out[map[i]] = in[i];
}

static void y2r_rotate_tile90(const uint32_t *in, uint32_t *out, int height,
                              const uint8_t *map) {
    int out_i = 0, x, y;
    for (x = 0; x < 8; ++x)
        for (y = height - 1; y >= 0; --y)
            out[map[out_i++]] = in[y * 8 + x];
}

static void y2r_rotate_tile180(const uint32_t *in, uint32_t *out, int height,
                               const uint8_t *map) {
    int out_i = 0, i;
    for (i = height * 8 - 1; i >= 0; --i) out[map[out_i++]] = in[i];
}

static void y2r_rotate_tile270(const uint32_t *in, uint32_t *out, int height,
                               const uint8_t *map) {
    int out_i = 0, x, y;
    for (x = 8 - 1; x >= 0; --x)
        for (y = 0; y < height; ++y)
            out[map[out_i++]] = in[y * 8 + x];
}

static void y2r_write_tile(uint32_t *out, const uint32_t *tile, int height,
                           int line_stride) {
    int x, y;
    for (y = 0; y < height; ++y)
        for (x = 0; x < 8; ++x)
            out[y * line_stride + x] = tile[y * 8 + x];
}

/* SendData<output_format>: schreibt `amount` RGB32-Pixel in DMA-Einheiten
 * von `transfer_unit` Bytes, dazwischen `gap` Bytes. */
static void y2r_send_data(const Y2rMemory *m, const uint32_t *in, Y2rBuffer *buf,
                          int amount, uint8_t alpha, int format) {
    uint32_t address = buf->address;
    while (amount > 0) {
        uint32_t off = 0;
        while (off < buf->transfer_unit) {
            uint32_t color = *in++;
            uint8_t r = (uint8_t)(color >> 24);
            uint8_t g = (uint8_t)(color >> 16);
            uint8_t b = (uint8_t)(color >> 8);
            switch (format) {
                case Y2R_OUT_RGBA8:
                    y2r_wr8(m, address + off + 0, alpha);
                    y2r_wr8(m, address + off + 1, b);
                    y2r_wr8(m, address + off + 2, g);
                    y2r_wr8(m, address + off + 3, r);
                    off += 4;
                    break;
                case Y2R_OUT_RGB8:
                    y2r_wr8(m, address + off + 0, b);
                    y2r_wr8(m, address + off + 1, g);
                    y2r_wr8(m, address + off + 2, r);
                    off += 3;
                    break;
                case Y2R_OUT_RGB5A1: {
                    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 3) << 6) |
                                            ((b >> 3) << 1) | (alpha >> 7));
                    y2r_wr8(m, address + off + 0, (uint8_t)(v & 0xFF));
                    y2r_wr8(m, address + off + 1, (uint8_t)(v >> 8));
                    off += 2;
                    break;
                }
                default: { /* Y2R_OUT_RGB565 */
                    uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) |
                                            (b >> 3));
                    y2r_wr8(m, address + off + 0, (uint8_t)(v & 0xFF));
                    y2r_wr8(m, address + off + 1, (uint8_t)(v >> 8));
                    off += 2;
                    break;
                }
            }
            amount -= 1;
            if (amount == 0) break;
        }
        address += (uint32_t)buf->transfer_unit + buf->gap;
        buf->image_size -= buf->transfer_unit;
    }
    buf->address = address;
}

/* Arbeitspuffer (max. 1024x8 Pixel). Header-weit statisch, damit weder
 * Heap noch ein grosser Stackrahmen noetig ist; es laeuft immer nur ein
 * Gast-Thread gleichzeitig (kooperatives Scheduling). */
static uint32_t y2r_strip_words[Y2R_MAX_LINE_WIDTH * 8u];
static uint32_t y2r_tile_buffer[Y2R_MAX_TILES * Y2R_TILE_SIZE];
static uint32_t y2r_tmp_tile[Y2R_TILE_SIZE];

/* Fuehrt die Konfiguration aus (Azahar HW::Y2R::PerformConversion).
 * Rueckgabe 0 = ok, <0 = ungueltige Konfiguration (Azahar-ASSERT-Faelle;
 * die Hardware liefert dort kein Completion-Interrupt). */
static int y2r_perform_conversion(const Y2rMemory *m, Y2rConversion *cvt) {
    const uint8_t *tile_map;
    unsigned width, lines, num_tiles, y;
    uint8_t *strip = (uint8_t *)y2r_strip_words;
    uint32_t *tiles = y2r_tile_buffer;
    uint32_t *out_tile = y2r_tmp_tile;

    width = cvt->input_line_width;
    lines = cvt->input_lines;
    if (width == 0 || width > Y2R_MAX_LINE_WIDTH || (width % 8u) != 0) return -1;
    if (cvt->block_alignment == Y2R_ALIGN_BLOCK8X8 && (lines % 8u) != 0) return -1;
    if (cvt->input_format < Y2R_IN_YUV422_INDIV8 ||
        cvt->input_format > Y2R_IN_YUYV422_INTERLEAVED) return -1;
    if (cvt->output_format < Y2R_OUT_RGBA8 || cvt->output_format > Y2R_OUT_RGB565) return -1;
    num_tiles = width / 8u;
    if (num_tiles > Y2R_MAX_TILES) return -1;
    /* DMA-Einheiten muessen gesetzt sein (sonst kein Fortschritt moeglich). */
    if (cvt->dst.transfer_unit == 0) return -1;
    if (cvt->input_format == Y2R_IN_YUYV422_INTERLEAVED) {
        if (cvt->src_yuyv.transfer_unit == 0) return -1;
    } else {
        if (cvt->src_y.transfer_unit == 0 || cvt->src_u.transfer_unit == 0 ||
            cvt->src_v.transfer_unit == 0) return -1;
    }

    tile_map = cvt->block_alignment == Y2R_ALIGN_BLOCK8X8 ? y2r_morton_lut
                                                          : y2r_linear_lut;

    for (y = 0; y < lines; y += 8u) {
        unsigned row_height = lines - y < 8u ? lines - y : 8u;
        uint32_t row_data_size = row_height * width;
        uint8_t *in_y = strip;
        uint8_t *in_u = in_y + 8u * width;
        uint8_t *in_v = in_u + (8u * width) / 2u;
        uint32_t *out = y2r_strip_words;
        unsigned i;
        int format = cvt->input_format;

        memset(tiles, 0, num_tiles * Y2R_TILE_SIZE * sizeof(uint32_t));
        if (format == Y2R_IN_YUV422_INDIV8) {
            y2r_receive_data(m, in_y, &cvt->src_y, row_data_size, 1);
            y2r_receive_data(m, in_u, &cvt->src_u, row_data_size / 2u, 1);
            y2r_receive_data(m, in_v, &cvt->src_v, row_data_size / 2u, 1);
            y2r_convert_strip(in_y, in_u, in_v, tiles, format, width, row_height,
                              cvt->coefficients);
        } else if (format == Y2R_IN_YUV420_INDIV8) {
            y2r_receive_data(m, in_y, &cvt->src_y, row_data_size, 1);
            y2r_receive_data(m, in_u, &cvt->src_u, row_data_size / 4u, 1);
            y2r_receive_data(m, in_v, &cvt->src_v, row_data_size / 4u, 1);
            y2r_convert_strip(in_y, in_u, in_v, tiles, format, width, row_height,
                              cvt->coefficients);
        } else if (format == Y2R_IN_YUV422_INDIV16) {
            y2r_receive_data(m, in_y, &cvt->src_y, row_data_size, 2);
            y2r_receive_data(m, in_u, &cvt->src_u, row_data_size / 2u, 2);
            y2r_receive_data(m, in_v, &cvt->src_v, row_data_size / 2u, 2);
            y2r_convert_strip(in_y, in_u, in_v, tiles, format, width, row_height,
                              cvt->coefficients);
        } else if (format == Y2R_IN_YUV420_INDIV16) {
            y2r_receive_data(m, in_y, &cvt->src_y, row_data_size, 2);
            y2r_receive_data(m, in_u, &cvt->src_u, row_data_size / 4u, 2);
            y2r_receive_data(m, in_v, &cvt->src_v, row_data_size / 4u, 2);
            y2r_convert_strip(in_y, in_u, in_v, tiles, format, width, row_height,
                              cvt->coefficients);
        } else {
            /* YUYV422_Interleaved: ein Strom, U/V stehen zwischen den Ys. */
            y2r_receive_data(m, in_y, &cvt->src_yuyv, row_data_size * 2u, 1);
            y2r_convert_strip(in_y, NULL, NULL, tiles, format, width, row_height,
                              cvt->coefficients);
        }

        /* Kacheln rotieren und in den Ausgabepuffer des Streifens legen.
         * Der Eingabestreifen ist ab hier verbraucht; Azahar nutzt denselben
         * Puffer als Ausgabe (data_buffer). */
        for (i = 0; i < num_tiles; ++i) {
            int strip_width, stride;
            switch (cvt->rotation) {
                case Y2R_ROT_CW90:
                    y2r_rotate_tile90(tiles + i * Y2R_TILE_SIZE, out_tile,
                                      (int)row_height, tile_map);
                    strip_width = 8;
                    stride = 8 * (int)row_height;
                    break;
                case Y2R_ROT_CW180:
                    y2r_rotate_tile180(tiles + (num_tiles - i - 1) * Y2R_TILE_SIZE,
                                       out_tile, (int)row_height, tile_map);
                    strip_width = (int)width;
                    stride = 8;
                    break;
                case Y2R_ROT_CW270:
                    y2r_rotate_tile270(tiles + (num_tiles - i - 1) * Y2R_TILE_SIZE,
                                       out_tile, (int)row_height, tile_map);
                    strip_width = 8;
                    stride = 8 * (int)row_height;
                    break;
                default:
                    y2r_rotate_tile0(tiles + i * Y2R_TILE_SIZE, out_tile,
                                     (int)row_height, tile_map);
                    strip_width = (int)width;
                    stride = 8;
                    break;
            }
            if (cvt->block_alignment == Y2R_ALIGN_BLOCK8X8) {
                y2r_write_tile(out, out_tile, 8, 8);
                out += Y2R_TILE_SIZE;
            } else {
                y2r_write_tile(out, out_tile, (int)row_height, strip_width);
                out += stride;
            }
        }

        y2r_send_data(m, y2r_strip_words, &cvt->dst, (int)row_data_size,
                      (uint8_t)cvt->alpha, cvt->output_format);
    }
    return 0;
}

#endif /* Y2R_H */
