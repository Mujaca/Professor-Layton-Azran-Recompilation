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
    float quat[4];   /* QUATERNION (Fragment-Lighting, unnormiert) */
    float view[3];   /* VIEW (Fragment-Lighting) */
    float tc0_w;     /* TEXCOORD0_W */
} HwVertex;

/* Fragment-Lighting je Lichtquelle (Werte exakt wie LightSlot in gpu.c,
 * dort einmal je Draw aus 0x140..0x1D9 berechnet). */
typedef struct {
    float position[3];
    float spot_dir[3];
    float spec0[3], spec1[3], diffuse[3], ambient[3];
    float dist_scale, dist_bias;
    uint32_t num;               /* Lichtnummer (Spot-LUT 8+num, Dist-LUT 16+num) */
    uint32_t lcfg;              /* LightSrc::config */
    uint32_t enables;           /* HW_LIGHT_* */
} HwLight;

#define HW_LIGHT_DIST     (1u << 0)
#define HW_LIGHT_SPOT     (1u << 1)
#define HW_LIGHT_D0       (1u << 2)
#define HW_LIGHT_D1       (1u << 3)
#define HW_LIGHT_FRESNEL  (1u << 4)
#define HW_LIGHT_SHADOW_P (1u << 5)
#define HW_LIGHT_SHADOW_S (1u << 6)
#define HW_LIGHT_RR       (1u << 7)
#define HW_LIGHT_RG       (1u << 8)
#define HW_LIGHT_RB       (1u << 9)

typedef struct {
    int enabled;                /* 0x1C6 Bit0 == 0 */
    uint32_t config0, config1;  /* 0x1C3/0x1C4 */
    uint32_t config;            /* config0 Bits4..7 (LightingConfig) */
    uint32_t nlights;           /* max_light_index + 1 */
    float gal[3];               /* global_ambient */
    /* Fuer alle Lichter gleich (0x1D0..0x1D2): Eingaenge je Sampler in der
     * Reihenfolge Spot, D0, D1, Fresnel, RR, RG, RB (je 4 Bit), Abs-Flag je
     * Sampler (Bit i), Skalierung je Sampler. */
    uint32_t inputs;
    uint32_t abs_bits;
    float scale[7];
    HwLight light[8];
    /* LUTs (24 x 256, Wert + Differenz als float, exakt wie im
     * Software-Pfad vorberechnet); lut_gen aendert sich bei jedem Schreiben. */
    const float *lut_val;
    const float *lut_diff;
    const uint32_t *lut_rowgen; /* Aenderungszaehler je LUT (24) */
    uint32_t lut_gen;
} HwLighting;

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

    /* Fragment-Lighting 0x140..0x1D9 */
    HwLighting light;
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

/* Serie des zuletzt vollstaendig uebernommenen Zustands (hw_begin_draw).
 * hw_continue_draw(serie) == 1 heisst: der Zustand dieser Serie ist noch
 * gebunden und gueltig (keine Invalidierung, gleiche Texturgeneration);
 * gpu.c spart sich dann Aufbau und Vergleich des HwDrawState je Dreieck. */
unsigned hw_draw_serial(void);
int hw_continue_draw(unsigned serial);

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
/* Selbsttest: Pixelabdeckung eines Dreiecks nach den D3D11-Rasterregeln, wie
 * die Ueberlappungspruefung beim Batching sie rechnet (mask: w*h Bytes,
 * 1 = abgedeckt). Rueckgabe: Zahl der abgedeckten Pixel. */
int hw_cov_tri_mask(const HwVertex t[3], float half_w, float half_h,
                    int corner_x, int corner_y, int w, int h,
                    unsigned char *mask);
/* Textur-Upload: Kachelzeilen [trow0, trow1) eines PICA-Texturbilds der
 * Breite w nach RGBA8 (out = Pixelzeile trow0*8, Zeilenabstand w*4 Bytes). */
void hw_tex_decode_rows(const unsigned char *src, uint32_t fmt, uint32_t w,
                        uint32_t trow0, uint32_t trow1, unsigned char *out);
/* Uploads, die nur die geaenderten Kachelzeilen ersetzt haben. */
unsigned long long hw_texture_partial_uploads(void);
/* D3D-Draws und vorzeitige Uebergaben an die GPU (ID3D11DeviceContext::
 * Flush alle HW_SUBMIT_BATCHES Draws, damit die GPU nicht erst beim
 * Rueckschreiben anfaengt). */
void hw_submit_stats(unsigned long long *batches, unsigned long long *submits);

/* ---- M17: interne Aufloesung und Anzeige auf der GPU -------------------
 * Gastsichtbare Ergebnisse (Rueckschreiben, Transfers) bleiben nativ und
 * unveraendert; die interne Aufloesung betrifft nur die Anzeige.
 * hw_set_scale vor dem ersten Draw aufrufen (1..4, Standard 1). */
void hw_set_scale(int s);
int hw_scale(void);
/* Nach dem CPU-DisplayTransfer (gpu.c): denselben Transfer auf der GPU aus
 * der Oberflaeche der Quelle rechnen (falls vorhanden und gueltig).
 * src/dst = Host-Zeiger in den Gastspeicher, Groessen/Flags wie
 * Register 0x302..0x304, extent = beschriebene Bytes ab dst. */
void hw_display_transfer(const unsigned char *src, const unsigned char *dst,
                         uint32_t in_size, uint32_t out_size, uint32_t flags,
                         uint32_t extent);
/* Fuer runtime/gpu/hw_present.c: Context-Sperre, Device/Context
 * (ID3D11Device* / ID3D11DeviceContext*) und die Anzeigeflaeche
 * (ID3D11ShaderResourceView*, Gast-Orientierung: Texel (x, y) = Pixel x der
 * Framebuffer-Zeile y) zu einem Framebuffer, nur solange dessen Gastbytes
 * dem GPU-Transfer entsprechen. lookup nur unter hw_lock(). */
void hw_lock(void);
void hw_unlock(void);
void *hw_d3d_device(void);
void *hw_d3d_context(void);
void *hw_display_lookup(const unsigned char *ptr, uint32_t stride, uint32_t fmt,
                        int columns, int rows, int *tex_w, int *tex_h);
void hw_display_stats(unsigned long long *gpu, unsigned long long *cpu);
/* HLSL uebersetzen (ID3DBlob* oder NULL). */
void *hw_compile_shader(const char *src, const char *entry, const char *profile);

#endif
