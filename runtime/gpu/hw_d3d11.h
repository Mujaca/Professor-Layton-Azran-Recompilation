#ifndef HW_D3D11_H
#define HW_D3D11_H

#include <stdint.h>

/* D3D11-Rasterizer fuer die PICA-Pipeline (Meilenstein M9, docs/IDEAS.md).
 *
 * Architektur (verbindlich): Vertex- und Geometry-Shader laufen weiter auf
 * der CPU (runtime/gpu/gpu.c). Dieses Modul uebernimmt nur die Rasterisierung
 * und die Fragment-Pipeline (Texturen + TEV + Alpha-Test + Output-Merger
 * inkl. Blending/LogicOp/Farbmaske + Stencil + Fog):
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

    /* Output-Merger 0x100..0x107/0x113. Faktoren/Gleichungen mit den
     * PICA-Enumerationswerten aus Azahar regs_framebuffer.h; die D3D-
     * Uebersetzung passiert in hw_d3d11.c. */
    int blend_enable;
    uint32_t blend_eq_rgb, blend_eq_a;      /* 0 Add,1 Sub,2 RevSub,3 Min,4 Max */
    uint32_t blend_src_rgb, blend_dst_rgb;  /* 0..14 */
    uint32_t blend_src_a, blend_dst_a;
    uint32_t blend_const;                   /* 0xAABBGGRR */
    uint32_t logic_op;                      /* 0..15, 3 = Copy */
    uint32_t color_write_mask;              /* Bit0..3 = r,g,b,a */
    int color_write_enable;                 /* 0x113 & 0xF != 0 */

    /* Tiefen-/Stenciltest 0x105/0x106 (Stencil nur bei D24S8) */
    int stencil_enable;
    uint32_t stencil_func, stencil_ref, stencil_input_mask, stencil_write_mask;
    uint32_t stencil_fail_op, stencil_zfail_op, stencil_pass_op;

    /* Fog 0xE0/0xE1/0xE6/0xE8..0xEF (LUT-Schnappschuss) */
    int fog_mode;
    int fog_flip;
    uint32_t fog_color;
    uint32_t fog_lut[128];

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
    /* Feld bleibt aus Kompatibilitaet; hw_begin_draw liest die Generation
     * selbst NACH dem Flush eines Zielwechsels (Render-to-Texture). */
    uint32_t tex_generation;
    HwTexture tex[3];
} HwDrawState;

/* Device: NULL = Hardware-Treiber, Fallback WARP; "warp" = erzwungen WARP. */
int hw_init(const char *driver);
void hw_shutdown(void);
int hw_available(void);

/* Zustand uebernehmen; bei geaendertem Ziel wird vorher geflusht und der
 * Gastinhalt neu geladen. Rueckgabe 0 bei Fehler. */
int hw_begin_draw(const HwDrawState *state);

/* Zu Beginn jeder GX-Kommandoliste aufrufen: prueft per memcmp gegen eine
 * Schattenkopie, ob die Gast-CPU Farb-/Tiefenziel zwischen den Listen
 * geaendert hat. Nur dann wird das Ziel verworfen und beim naechsten Draw
 * neu aus dem Gast geladen (sonst wuerde der Flush die CPU-Bytes mit dem
 * alten D3D-Stand ueberschreiben). */
void hw_list_begin(void);

/* n Dreiecke (n Vielfaches von 3). Rueckgabe 0 bei Fehler. */
int hw_triangles(const HwVertex *v, int n);

/* Farb-/Tiefenpuffer ins Gast-VRAM schreiben (nur wenn seit dem letzten
 * Schreiben wirklich GPU-Ergebnisse ausstehen). */
int hw_flush_to_guest(void);

/* Vor einer Gast-Schreiboperation (MemoryFill, DisplayTransfer,
 * TextureCopy, Software-Draw): ausstehende GPU-Ergebnisse zuerst ins
 * Gast-VRAM schreiben und danach den HW-Zielzustand verwerfen, damit der
 * naechste HW-Draw das Ziel neu aus dem Gast laedt. */
void hw_sync_guest(void);

/* Signalisieren, dass der Gast den Puffer bereits veraendert hat: den
 * HW-Zielzustand verwerfen, ohne ihn zurueckzuschreiben. */
void hw_invalidate_guest(void);

/* Rueckschreibepolitik (Ausbaustufe A): liegen ausstehende GPU-Ergebnisse
 * vor und ueberlappt [offset, offset+bytes) das aktuelle Farb-/Tiefenziel,
 * wird zuerst zurueckgeschrieben. offset bezieht sich auf
 * HwDrawState.guest_mem. Sonst passiert nichts. */
void hw_sync_range(uint32_t offset, uint32_t bytes);
/* Der Gast beschreibt [offset, offset+bytes): nur bei Ueberlappung mit dem
 * aktuellen Ziel wird gesichert und der D3D-Zielzustand verworfen. */
void hw_guest_write_range(uint32_t offset, uint32_t bytes);
/* 1 = es stehen GPU-Ergebnisse aus, die noch nicht im Gast stehen. */
int hw_target_pending(void);

/* Signalisieren, dass sich Gast-Texturbytes geaendert haben. */
void hw_invalidate_textures(void);

/* Nur Texturen, die den Gastbereich [offset, offset+bytes) ueberlappen,
 * beim naechsten Draw neu pruefen (GX-Schreiboperationen). */
void hw_invalidate_texture_range(uint32_t offset, uint32_t bytes);

/* Diagnose: benutzter Treiber ("hardware"/"warp"), Draws, Flushes,
 * Textur-Uploads. */
const char *hw_driver_name(void);
void hw_stats(unsigned long long *draws, unsigned long long *flushes);
/* hw_list_begin: gehaltene (Gast unveraendert) / neu geladene Ziele. */
void hw_target_sync_stats(unsigned long long *kept, unsigned long long *reloads);
unsigned long long hw_texture_uploads(void);
/* Diagnose: Uploads und Wandzeit der Dekodierung (HLE_GTIME/HLE_HW_STATE). */
void hw_texture_stats(unsigned long long *uploads, unsigned long long *decode_us);
unsigned long long hw_depth_writes(void);
/* Volle Gast->D3D-Ladungen des Ziels (Diagnose fuer HW/SW-Wechsel). */
unsigned long long hw_target_loads(void);

#endif
