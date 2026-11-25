#ifndef HW_D3D11_H
#define HW_D3D11_H

#include <stdint.h>

/* D3D11-Rasterizer fuer die PICA-Pipeline (Meilenstein M9, docs/IDEAS.md).
 *
 * Architektur (verbindlich): Vertex- und Geometry-Shader laufen weiter auf
 * der CPU (runtime/gpu/gpu.c). Dieses Modul uebernimmt nur die Rasterisierung
 * und die Fragment-Pipeline (Texturen + TEV + Alpha-Test):
 *
 *   hw_begin_draw(state)  Ziel/Viewport/Scissor/Tiefe/TEV setzen und
 *                         Texturen bei Bedarf hochladen
 *   hw_triangles(v, n)    Dreiecke in Clip-Space (nach VS/GS, vor Clipping
 *                         bereits in gpu.c geclippt) mit Ausgangs-Attributen
 *   hw_flush_to_guest()   Farb-/Tiefenpuffer ins Gast-VRAM zurueckschreiben
 *                         (PICA-Format, 8x8-Morton, von unten nach oben)
 *
 * Der Software-Rasterizer bleibt Referenz und Standard. Eingeschaltet wird
 * das Modul ausschliesslich per Kommandozeile `--renderer hw` (runtime/main.c).
 */

typedef struct {
    float pos[4];    /* Clip-Space (x,y,z,w), PICA-Konvention z in [-w,0] */
    float color[4];  /* PrimaryColor (bereits abs/saturiert) */
    float uv[3][2];  /* TEXCOORD0..2 (D3D interpoliert perspektivkorrekt) */
} HwVertex;

/* Eine adressierte Textur (Einheit 0..2). offset/bytes beziehen sich auf
 * HwDrawState.guest_mem; das Modul dekodiert die Kacheln selbst. */
typedef struct {
    int enabled;                /* main_config 0x80 Bit der Einheit */
    uint32_t offset;            /* Gast-linearer Offset des Texturbildes */
    uint32_t bytes;             /* Kacheln * Kachelgroesse (0 = ungueltig) */
    uint32_t format;            /* PICA-Texturformat 0..13 */
    uint32_t width, height;
    uint32_t wrap_s, wrap_t;    /* 0..7 wie p_wrap_coord */
    uint32_t border_rgba;       /* 0xAABBGGRR (0x81) */
    uint32_t filter;            /* 0 Nearest, 1 Linear */
    uint32_t type;              /* 0 Texture2D, 5 Disabled */
} HwTexture;

typedef struct {
    /* Farbziel (Gastzeiger, bereits physisch->linear umgerechnet) */
    unsigned char *color_mem;
    uint32_t color_size;        /* nutzbare Bytes ab color_mem */
    uint32_t color_width;
    uint32_t color_height;
    uint32_t color_format;      /* 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565, 4 RGBA4 */

    /* Tiefenziel (0x11C); depth_mem == NULL = kein Rueckschreiben */
    unsigned char *depth_mem;
    uint32_t depth_size;
    uint32_t depth_format;      /* 0 D16, 2 D24, 3 D24S8 */
    int depth_test;
    int depth_write;
    uint32_t depth_func;        /* PICA 0..7 (Never..GreaterEqual) */

    /* Viewport 0x41/0x43/0x68, Tiefenbereich 0x4D/0x4E */
    float viewport_half_w;
    float viewport_half_h;
    int viewport_corner_x;
    int viewport_corner_y;
    float depth_range;
    float depth_near;

    /* Scissor 0x65/0x66/0x67 (Pixelkoordinaten, x2/y2 exklusiv) */
    uint32_t scissor_mode;      /* 3 = Include; andere Modi: kein Scissor */
    int scissor_x1, scissor_y1, scissor_x2, scissor_y2;

    /* Cull 0x40 Bits0..1 (PICA-Signatur, CPU-seitig geprueft in gpu.c) */
    uint32_t cull;

    /* Alpha-Test 0x104 */
    int alpha_test;
    uint32_t alpha_func;
    uint32_t alpha_ref;

    /* TEV 0xC0..0xFD: je Stufe sources/modifiers/ops/const_color/scales,
     * dazu Combiner-Buffer-Maske (0xE0) und Pufferfarbe (0xFD). Semantik
     * exakt wie p_tev im Software-Pfad. */
    uint32_t tev_src[6];
    uint32_t tev_mod[6];
    uint32_t tev_ops[6];
    uint32_t tev_cst[6];
    uint32_t tev_scl[6];
    uint32_t tev_buf_in;
    uint32_t tev_buf_color;

    /* Texturzustand; guest_mem != NULL ist Voraussetzung fuer Texturen. */
    unsigned char *guest_mem;
    uint32_t guest_size;
    uint32_t tex_generation;    /* gpu_pica_tex_generation() */
    HwTexture tex[3];
} HwDrawState;

/* Device: NULL = Hardware-Treiber, Fallback WARP; "warp" = erzwungen WARP. */
int hw_init(const char *driver);
void hw_shutdown(void);
int hw_available(void);

/* Zustand uebernehmen; bei geaendertem Ziel wird vorher geflusht und der
 * Gastinhalt neu geladen. Rueckgabe 0 bei Fehler. */
int hw_begin_draw(const HwDrawState *state);

/* n Dreiecke (n Vielfaches von 3). Rueckgabe 0 bei Fehler. */
int hw_triangles(const HwVertex *v, int n);

/* Farb-/Tiefenpuffer ins Gast-VRAM schreiben. */
int hw_flush_to_guest(void);

/* Signalisieren, dass der Software-Pfad den Gastpuffer veraendert hat. */
void hw_invalidate_guest(void);

/* Signalisieren, dass sich Gast-Texturbytes geaendert haben. */
void hw_invalidate_textures(void);

/* Diagnose: benutzter Treiber ("hardware"/"warp"), Draws, Flushes,
 * Textur-Uploads. */
const char *hw_driver_name(void);
void hw_stats(unsigned long long *draws, unsigned long long *flushes);
unsigned long long hw_texture_uploads(void);

#endif
