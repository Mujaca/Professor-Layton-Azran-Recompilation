/* PICA-Registermap- und Kommandolisten-Test (ohne Gast-Code).
 *
 * Baut synthetische PICA-Kommandolisten und prueft:
 *   0x200  Vertex-Basis (base = (reg>>1)*16)
 *   0x201  Attributformate (attr0 float3, attr1 float2)
 *   0x203+ Loader (byte_count = Stride 16 im Wort 2)
 *   0x228  num_vertices, 0x22F Draw-Trigger
 *   0x80   Textur-Enable ist Bit 0 von main_config
 *   0x82/0x85/0x8E Groesse/Adresse/Format
 *
 * Zusaetzlich (Semantik aus Azahar pica_core.cpp ProcessCmdList/WriteInternalReg):
 *   - Header-Felder id/mask/extra/group
 *   - Byte-Maske (nur markierte Bytes schreiben)
 *   - Consecutive-Writes: group=1 (id+1..id+n) und group=0 (id n-mal)
 *   - 8-Byte-Ausrichtung nach ungeraden Records
 *   - 0x010 Finalize: P3D-Interrupt + irq_autostop
 *   - 0x238/0x23A/0x23C Befehlspuffer-Sprung (ersetzt die laufende Liste)
 *   - physisch -> Gast-Linearspeicher (VRAM 0x18000000, FCRAM 0x20000000)
 *
 * Erwartung: das Dreieck wird mit der Textur (rot) gefuellt; nach
 * 0x80=0 liefert die deaktivierte Einheit (0,0,0,0) wie Azahar.
 * HLE_PICA_RENDER=1 setzt der Runner (tools/test_pica_c.py).
 *
 * Ergaenzt (Azahar texture_decode.cpp/sw_texturing.cpp/sw_framebuffer.cpp):
 * 8x8-Morton-Texturen in allen 14 Formaten, Wrap-Modi, Nearest-Sampling,
 * ETC1-Subtiles, Farbziel 0x117/0x11D/0x11E, LogicOps, Alpha-/Depth-/
 * Stencil-Test und Blending.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "arm_cpu.h"
#include "gpu.h"

/* Stubs: arm_cpu.c referenziert die Trace-Runtime. */
uint32_t arm_last_block = 0;
void arm_trace_svc(uint32_t imm) { (void)imm; }

#define FB 0x00030000u
#define TEX 0x00020000u
#define VTX 0x00021000u
#define LIST 0x00001000u
#define LIST2 0x00005000u
#define VTX3 0x00023000u

static int fails = 0;

static void check(int cond, const char *what) {
    printf("%s %s\n", cond ? "OK  " : "FEHL", what);
    if (!cond) fails++;
}

static void wr32(ArmCpu *c, uint32_t a, uint32_t v) {
    memcpy(c->mem + (a & 0x1FFFFFFFu), &v, 4);
}

static uint32_t *emit_h(uint32_t *p, uint32_t val, uint32_t id, uint32_t mask) {
    p[0] = val;
    p[1] = (id & 0xFFFFu) | ((mask & 0xFu) << 16);
    return p + 2;
}

static uint32_t *emit(uint32_t *p, uint32_t id, uint32_t val) {
    return emit_h(p, val, id, 0xFu);
}

static int irq_count = 0;
static void test_irq(int id) { if (id == 5) irq_count++; }

static void set_vertex(ArmCpu *c, uint32_t off, float x, float y, float z,
                       float u, float v) {
    float f[5];
    f[0] = x; f[1] = y; f[2] = z; f[3] = u; f[4] = v;
    memcpy(c->mem + ((VTX + off) & 0x1FFFFFFFu), f, sizeof(f));
}

/* float24 (Pica::Float<16,7>) fuer Testvektoren; Azahar FromRaw-Format:
   Bit23 Sign, Bits16..22 Exponent (Bias 64), Bits0..15 Mantisse. */
static uint32_t f24raw(float v)
{
    int e;
    float m;
    uint32_t sign, mant, ex;
    if (v == 0.0f) return 0u;
    m = frexpf(v < 0.0f ? -v : v, &e);   /* v = m*2^e, m in [0.5,1) */
    sign = (v < 0.0f) ? 0x800000u : 0u;
    mant = (uint32_t)((m * 2.0f - 1.0f) * 65536.0f + 0.5f) & 0xFFFFu;
    ex = (uint32_t)(e - 1 + 63);   /* PICA-f24-Bias 63 */
    return sign | (ex << 16) | mant;
}

/* Drei gepackte Woerter fuer Vec4<f24> (Azahar PackedAttribute::AsFloat24). */
static void f24_pack3(float x, float y, float z, float w, uint32_t out[3])
{
    uint32_t X = f24raw(x), Y = f24raw(y), Z = f24raw(z), W = f24raw(w);
    out[0] = (W << 8) | (Z >> 16);
    out[1] = ((Z & 0xFFFFu) << 16) | (Y >> 8);
    out[2] = ((Y & 0xFFu) << 24) | X;
}

/* Instruktionskodierung nach nihstro shader_bytecode.h. */
static uint32_t ins_common(uint32_t op, uint32_t desc, uint32_t src1,
                           uint32_t src2, uint32_t dest, uint32_t ari)
{
    return (op << 26) | (desc & 0x7Fu) | ((src2 & 0x1Fu) << 7) |
           ((src1 & 0x7Fu) << 12) | ((ari & 3u) << 19) | ((dest & 0x1Fu) << 21);
}

/* SrcInversed-Format (nihstro Info::SrcInversed): DPHI 0x18, DSTI 0x19,
   SGEI 0x1A, SLTI 0x1B. src1i = Bits14..18 (5 Bit), src2i = Bits7..13
   (7 Bit); nur src2i kann eine Float-Uniform adressieren. */
static uint32_t ins_common_inv(uint32_t op, uint32_t desc, uint32_t src1i,
                               uint32_t src2i, uint32_t dest, uint32_t ari)
{
    return (op << 26) | (desc & 0x7Fu) | ((src2i & 0x7Fu) << 7) |
           ((src1i & 0x1Fu) << 14) | ((ari & 3u) << 19) |
           ((dest & 0x1Fu) << 21);
}

static uint32_t ins_flow(uint32_t op, uint32_t num, uint32_t dest,
                         uint32_t bid, uint32_t refx, uint32_t refy)
{
    return (op << 26) | (num & 0xFFu) | ((dest & 0xFFFu) << 10) |
           ((bid & 0xFu) << 22) | ((refy & 1u) << 24) | ((refx & 1u) << 25);
}

/* Volles Swizzle-Muster xyzw fuer Quelle 1 und 2, Dest-Maske xyzw. */
static uint32_t swz_xyzw(void)
{
    return 0xFu | (1u << 9) | (2u << 7) | (3u << 5) |
           (1u << 18) | (2u << 16) | (3u << 14);
}

static uint32_t *emit_program(uint32_t *p, const uint32_t *words, int n)
{
    int i;
    p = emit(p, 0x2CB, 0);
    for (i = 0; i < n; i++) p = emit(p, 0x2CC, words[i]);
    return p;
}

static uint32_t *emit_swizzle0(uint32_t *p, uint32_t w)
{
    p = emit(p, 0x2D5, 0);
    return emit(p, 0x2D6, w);
}

/* Viewport 0x41/0x43 (halbe Groesse, float24) + Ecke 0x68. */
static uint32_t *emit_viewport(uint32_t *p, int w, int h)
{
    p = emit(p, 0x41, f24raw((float)w * 0.5f));
    p = emit(p, 0x43, f24raw((float)h * 0.5f));
    return emit(p, 0x68, 0u);
}

/* TEV-Stufe 0: Farbe = Quelle `src`, Alpha = Quelle `asrc`, Op Replace.
   sources: 0 PrimaryColor, 3 Texture0, 0xe Constant, 0xf Previous. */
static uint32_t *emit_tev0(uint32_t *p, uint32_t src, uint32_t asrc,
                           uint32_t const_rgba)
{
    int s;
    p = emit(p, 0xC0, (src & 0xFu) | ((asrc & 0xFu) << 16));
    p = emit(p, 0xC1, 0u);          /* keine Modifier */
    p = emit(p, 0xC2, 0u);          /* Replace, Alpha Replace */
    p = emit(p, 0xC3, const_rgba);  /* const_color */
    p = emit(p, 0xC4, 0u);          /* Skalierung 1x */
    /* Ungenutzte Stufen 1..5 als Durchreichen (Quelle Previous, Replace).
       Die Hardware/Azahar fuehrt immer alle 6 TEV-Stufen aus. */
    for (s = 1; s < 6; s++) {
        uint32_t base = (s < 4) ? (0xC0u + (uint32_t)s * 8u) : (0xF0u + (uint32_t)(s - 4) * 8u);
        p = emit(p, base + 0u, 0x000F000Fu);  /* color/alpha source1 = Previous */
        p = emit(p, base + 1u, 0u);
        p = emit(p, base + 2u, 0u);
        p = emit(p, base + 3u, 0u);
        p = emit(p, base + 4u, 0u);
    }
    return p;
}

/* Standard-Attributsetup: 1 Attribut (float3) aus VTX, Stride 16. */
static uint32_t *emit_attr0(uint32_t *p, uint32_t base)
{
    p = emit(p, 0x200, base >> 3);
    p = emit(p, 0x201, 0xBu);              /* attr0: float, 3 Komponenten */
    p = emit(p, 0x202, 0u);                /* max_attribute_index = 0 */
    p = emit(p, 0x203, 0u);                /* Loader 0: offset 0 */
    p = emit(p, 0x204, 0u);                /* comp0 = attr0 */
    p = emit(p, 0x205, (1u << 28) | (16u << 16));  /* cc=1, stride=16 */
    return p;
}

/* ================= Erweiterte PICA-Tests =================
 * Erwartungswerte aus Azahar: texture_decode.cpp (8x8-Morton-Kacheln,
 * Kanalreihenfolge), sw_texturing.cpp (Wrap), sw_framebuffer.cpp (Formate,
 * Blending, LogicOp), sw_rasterizer.cpp (Alpha-/Depth-/Stencil-Test). */

static uint32_t t_morton(int x, int y)
{
    static const uint32_t xl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t yl[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    return xl[x % 8] + yl[y % 8];
}

/* Byte-Offset eines Pixels im gekachelten Farbziel (Azahar sw_framebuffer). */
static uint32_t t_fb_off(uint32_t base, int x, int y, int w, int h, int bpp)
{
    int yp = (h - 1) - y;
    uint32_t i = t_morton(x, yp) + (uint32_t)(x & ~7) * 8u;
    return base + (i + (uint32_t)(yp & ~7) * (uint32_t)w) * (uint32_t)bpp;
}

/* Sechs Vertices fuer ein Quad ueber den Clip-Raum (2 Dreiecke),
   Position float3 + UV float2, Stride 20. */
static void t_quad_vertices(ArmCpu *c, uint32_t vtx, float z,
                            float u0, float v0, float u1, float v1)
{
    static const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    int i;
    for (i = 0; i < 6; i++) {
        float f[5];
        f[0] = xy[i][0];
        f[1] = xy[i][1];
        f[2] = z;
        f[3] = u0 + (xy[i][0] + 1.0f) * 0.5f * (u1 - u0);
        f[4] = v0 + (xy[i][1] + 1.0f) * 0.5f * (v1 - v0);
        memcpy(c->mem + ((vtx + (uint32_t)i * 20u) & 0x1FFFFFFFu), f, sizeof(f));
    }
}

/* Standard-Vertexprogramm: attr0 -> POSITION, attr1 -> TEXCOORD0.
   Ohne hochgeladenes Programm zeichnet Azahar nichts; alle Tests mit
   t_emit_quad brauchen daher dieses Programm. */
static uint32_t *t_emit_vs_basic(uint32_t *q)
{
    uint32_t w[3];
    q = emit(q, 0x2B9, 2u);                /* max_input_attribute_index */
    q = emit(q, 0x2BB, 0x210u);            /* attr0->v0, attr1->v1 */
    q = emit(q, 0x4F, 2u);
    q = emit(q, 0x50, 0x03020100u);        /* o0 -> POSITION */
    q = emit(q, 0x51, 0x1F1F0D0Cu);        /* o1 -> TEXCOORD0 (12/13) */
    q = emit(q, 0x2BD, 3u);
    w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);
    w[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);
    w[2] = 0x22u << 26;
    q = emit_program(q, w, 3);
    return emit_swizzle0(q, swz_xyzw());
}

static uint32_t *t_emit_quad(uint32_t *q, uint32_t vtx, int vw, int vh)
{
    q = emit(q, 0x200, vtx >> 3);
    q = emit(q, 0x201, 0x7B);              /* attr0 float3, attr1 float2 */
    q = emit(q, 0x202, 1u << 28);
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);
    q = emit(q, 0x205, (3u << 28) | (20u << 16));
    q = emit(q, 0x206, 12u);
    q = emit(q, 0x207, 1u);
    q = emit(q, 0x208, (1u << 28) | (20u << 16));
    q = emit(q, 0x228, 6u);
    q = t_emit_vs_basic(q);
    q = emit_viewport(q, vw, vh);
    return q;
}

static uint32_t *t_emit_fb(uint32_t *q, uint32_t addr, int w, int h, uint32_t fmt)
{
    q = emit(q, 0x117, (fmt & 7u) << 16);  /* color_format */
    q = emit(q, 0x11D, addr >> 3);         /* color_buffer_address */
    q = emit(q, 0x11E, (uint32_t)w | ((uint32_t)(h - 1) << 12));
    q = emit(q, 0x107, 0x00000F00u);
    q = emit(q, 0x102, 3u);                /* LogicOp Copy */
    q = emit(q, 0x113, 1u);                /* allow_color_write */
    return q;
}

static uint32_t *t_emit_tex(uint32_t *q, uint32_t addr, int w, int h, int fmt,
                            uint32_t cfg)
{
    q = emit(q, 0x80, 1u);                 /* texture0_enable */
    q = emit(q, 0x82, ((uint32_t)w << 16) | (uint32_t)h);
    q = emit(q, 0x83, cfg);
    q = emit(q, 0x85, addr >> 3);
    q = emit(q, 0x8E, (uint32_t)fmt);
    return q;
}

/* Ein Texel im Azahar-Layout kodieren (mo = Morton-Index in der Kachel). */
static void t_tex_encode(unsigned char *tile, int fmt, unsigned mo,
                         const unsigned char c[4])
{
    switch (fmt) {
    case 0:                                /* RGBA8: a,b,g,r */
        tile[mo * 4 + 0] = c[3]; tile[mo * 4 + 1] = c[2];
        tile[mo * 4 + 2] = c[1]; tile[mo * 4 + 3] = c[0];
        break;
    case 1:                                /* RGB8: b,g,r */
        tile[mo * 3 + 0] = c[2]; tile[mo * 3 + 1] = c[1]; tile[mo * 3 + 2] = c[0];
        break;
    case 2: case 3: case 4: {
        unsigned v;
        if (fmt == 2)
            v = ((c[0] >> 3) << 11) | ((c[1] >> 3) << 6) | ((c[2] >> 3) << 1) | (c[3] >> 7);
        else if (fmt == 3)
            v = ((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3);
        else
            v = ((c[0] >> 4) << 12) | ((c[1] >> 4) << 8) | ((c[2] >> 4) << 4) | (c[3] >> 4);
        tile[mo * 2 + 0] = (unsigned char)(v & 0xFFu);
        tile[mo * 2 + 1] = (unsigned char)(v >> 8);
        break;
    }
    case 5:                                /* IA8: a,i */
        tile[mo * 2 + 0] = c[3]; tile[mo * 2 + 1] = c[0];
        break;
    case 6:                                /* RG8: g,r */
        tile[mo * 2 + 0] = c[1]; tile[mo * 2 + 1] = c[0];
        break;
    case 7:
        tile[mo] = c[0];
        break;
    case 8:
        tile[mo] = c[3];
        break;
    case 9:                                /* IA4 */
        tile[mo] = (unsigned char)(((c[0] >> 4) << 4) | (c[3] >> 4));
        break;
    case 10: case 11: {
        unsigned v = (fmt == 10) ? (c[0] >> 4) : (c[3] >> 4);
        unsigned char *s = tile + mo / 2;
        if (mo & 1u) *s = (unsigned char)((*s & 0x0Fu) | (v << 4));
        else *s = (unsigned char)((*s & 0xF0u) | v);
        break;
    }
    default:
        break;
    }
}

static void test_textures(ArmCpu *c, unsigned char *mem)
{
    static const struct {
        int fmt; const char *name;
        unsigned char col[4]; unsigned char exp[4];
    } cases[] = {
        {0, "RGBA8",  {0x11, 0x22, 0x33, 0x44}, {0x11, 0x22, 0x33, 0x44}},
        {1, "RGB8",   {0x12, 0x34, 0x56, 0xFF}, {0x12, 0x34, 0x56, 0xFF}},
        {2, "RGB5A1", {0xF8, 0x08, 0x10, 0xFF}, {0xFF, 0x08, 0x10, 0xFF}},
        {3, "RGB565", {0xF8, 0xFC, 0x10, 0xFF}, {0xFF, 0xFF, 0x10, 0xFF}},
        {4, "RGBA4",  {0xFF, 0x22, 0x33, 0x44}, {0xFF, 0x22, 0x33, 0x44}},
        {5, "IA8",    {0x55, 0x55, 0x55, 0xAA}, {0x55, 0x55, 0x55, 0xAA}},
        {6, "RG8",    {0x12, 0x34, 0x00, 0xFF}, {0x12, 0x34, 0x00, 0xFF}},
        {7, "I8",     {0x77, 0x77, 0x77, 0xFF}, {0x77, 0x77, 0x77, 0xFF}},
        {8, "A8",     {0x00, 0x00, 0x00, 0x99}, {0x00, 0x00, 0x00, 0x99}},
        {9, "IA4",    {0x55, 0x55, 0x55, 0xAA}, {0x55, 0x55, 0x55, 0xAA}},
        {10, "I4",    {0x77, 0x77, 0x77, 0xFF}, {0x77, 0x77, 0x77, 0xFF}},
        {11, "A4",    {0x00, 0x00, 0x00, 0x33}, {0x00, 0x00, 0x00, 0x33}},
    };
    uint32_t *q, size;
    int ci, x, y;

    for (ci = 0; ci < (int)(sizeof(cases) / sizeof(cases[0])); ci++) {
        int fmt = cases[ci].fmt;
        uint32_t tile = 0x20000u;
        uint32_t ts = (uint32_t)(fmt == 0 ? 256 : fmt == 1 ? 192 :
                                 (fmt <= 6 ? 128 : (fmt <= 9 ? 64 : 32)));
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x200);
        memset(mem + tile, 0, ts);
        for (y = 0; y < 8; y++)
            for (x = 0; x < 8; x++)
                t_tex_encode(mem + tile, fmt, t_morton(x, y), cases[ci].col);
        t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_tex(q, tile, 8, 8, fmt, 0u);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);      /* RGBA8 */
        q = emit_tev0(q, 3u, 3u, 0u);              /* Texture0 */
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        {
            char msg[96];
            int ok = 1;
            for (y = 0; y < 8 && ok; y++)
                for (x = 0; x < 8; x++) {
                    uint32_t o = t_fb_off(0x40000u, x, y, 8, 8, 4);
                    if (mem[o + 3] != cases[ci].exp[0] || mem[o + 2] != cases[ci].exp[1] ||
                        mem[o + 1] != cases[ci].exp[2] || mem[o + 0] != cases[ci].exp[3]) {
                        ok = 0;
                        break;
                    }
                }
            sprintf(msg, "Textur %s (Morton 8x8), Pixel (x,y) == Texel (x,7-y)",
                    cases[ci].name);
            check(ok, msg);
        }
    }
}

static void test_texture_morton(ArmCpu *c, unsigned char *mem)
{
    /* Nur (1,2) rot und (6,5) gruen; alle anderen Texel schwarz. */
    uint32_t *q, size;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(mem + 0x20000u, 0, 256);
    mem[0x20000u + t_morton(1, 2) * 4 + 3] = 0xFF;   /* r */
    mem[0x20000u + t_morton(6, 5) * 4 + 2] = 0xFF;   /* g (byte 2) */
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_tex(q, 0x20000u, 8, 8, 0, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    {
        uint32_t a = t_fb_off(0x40000u, 1, 5, 8, 8, 4);   /* Texel (1,2) */
        uint32_t b = t_fb_off(0x40000u, 6, 2, 8, 8, 4);   /* Texel (6,5) */
        uint32_t z = t_fb_off(0x40000u, 0, 0, 8, 8, 4);
        check(mem[a + 3] == 0xFF && mem[a + 2] == 0 && mem[a + 1] == 0,
              "Morton: Texel (1,2) liegt bei Pixel (1,5)");
        check(mem[b + 3] == 0 && mem[b + 2] == 0xFF,
              "Morton: Texel (6,5) liegt bei Pixel (6,2)");
        check(mem[z + 3] == 0 && mem[z + 2] == 0 && mem[z + 1] == 0,
              "Morton: unbelegte Texel bleiben schwarz");
    }
}

static void test_texture_wrap(ArmCpu *c, unsigned char *mem)
{
    static const struct {
        uint32_t cfg; const char *name; unsigned exp_r;
    } cases[] = {
        {2u << 12, "Repeat", 0x40},
        {0u, "ClampToEdge", 0x30},
        {3u << 12, "MirroredRepeat", 0x30},
        {1u << 12, "ClampToBorder", 0xDD},
        {5u << 12, "ClampToBorder2 (negativ = Repeat)", 0x40},
    };
    int i;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        uint32_t *q, size, o;
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        memset(mem + 0x20000u, 0, 64);
        /* 2x2 I8; ein Tile (width/8 = 0 => stride 0). */
        mem[0x20000u + t_morton(0, 0)] = 0x10;
        mem[0x20000u + t_morton(1, 0)] = 0x20;
        mem[0x20000u + t_morton(0, 1)] = 0x30;
        mem[0x20000u + t_morton(1, 1)] = 0x40;
        t_quad_vertices(c, VTX, 0.0f, -1.0f, 0.0f, 2.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_tex(q, 0x20000u, 2, 2, 7, cases[i].cfg);
        q = emit(q, 0x81, 0xAABBCCDDu);          /* border_color */
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit_tev0(q, 3u, 3u, 0u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 0, 0, 8, 8, 4);
        {
            char msg[96];
            sprintf(msg, "Sampler-Wrap %s => 0x%02X", cases[i].name, cases[i].exp_r);
            check(mem[o + 3] == cases[i].exp_r, msg);
        }
    }
}

static void test_texture_filter(ArmCpu *c, unsigned char *mem)
{
    /* Azahar sw_rasterizer.cpp wertet Min-/Mag-Filter nicht aus; es wird
       immer der naechste Texel gelesen ("TODO: Apply the min and mag
       filters"). Beide Filterkonfigurationen liefern daher 0. */
    int pass;
    for (pass = 0; pass < 2; pass++) {
        uint32_t *q, size, o;
        uint32_t cfg = pass ? ((1u << 1) | (1u << 2)) : 0u;   /* linear / nearest */
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        memset(mem + 0x20000u, 0, 64);
        mem[0x20000u + t_morton(0, 0)] = 0x00;
        mem[0x20000u + t_morton(1, 0)] = 0xFF;
        t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_tex(q, 0x20000u, 2, 1, 7, cfg);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit_tev0(q, 3u, 3u, 0u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 3, 0, 8, 8, 4);
        check(mem[o + 3] == 0,
              pass ? "Sampler-Filter linear: Azahar filtert nicht => 0"
                   : "Sampler-Filter nearest: u=0.4375 => 0");
    }
}

/* ================= Prozedurale Texturen (Einheit 3) =================
 * Azahar sw_proctex.cpp + pica_core.cpp (LUT-Upload). Die Erwartungswerte
 * sind unabhaengig aus den Azahar-Formeln abgeleitet (scratch-Skript):
 * color_map/alpha_map = lineare Rampe value=i*32 (diff 0), Farbtabelle
 * color[i] = (i,0,0,255), width 128, offset 0. Rot = floor/round von
 * lut_coord*127. */

/* Wertetabelle laden: 0xAF = Tabellen-Nr/Startindex, danach je Datenwort
 * ein Eintrag (Azahar PicaCore::ProcTex inkrementiert den Index). */
static uint32_t *t_emit_proctex_lut(uint32_t *q, uint32_t table, uint32_t start,
                                    const uint32_t *values, int n)
{
    int i;
    q = emit(q, 0xAF, ((table & 0xFu) << 8) | (start & 0xFFu));
    for (i = 0; i < n; i++)
        q = emit(q, 0xB0u + (uint32_t)(i & 7), values[i]);
    return q;
}

/* Lineare Rampen fuer color_map/alpha_map (value=i*32) und Farbtabelle
 * (color[i]=i, Alpha 255); optional die Noise-LUT komplett auf 4095. */
static uint32_t *t_emit_proctex_ramps(uint32_t *q, int noise_lut_full)
{
    uint32_t buf[256];
    int i;

    if (noise_lut_full) {
        for (i = 0; i < 128; i++) buf[i] = 0x0FFFu;
        q = t_emit_proctex_lut(q, 0u, 0u, buf, 128);
    }
    for (i = 0; i < 128; i++) buf[i] = (uint32_t)i * 32u;
    q = t_emit_proctex_lut(q, 2u, 0u, buf, 128);
    q = t_emit_proctex_lut(q, 3u, 0u, buf, 128);
    memset(buf, 0, sizeof(buf));
    q = t_emit_proctex_lut(q, 5u, 0u, buf, 256);
    for (i = 0; i < 256; i++) buf[i] = (uint32_t)i | 0xFF000000u;
    q = t_emit_proctex_lut(q, 4u, 0u, buf, 256);
    return q;
}

/* Ein Quad mit konstantem (u,v) zeichnen; TEV-Quelle 6 = Texture3.
   Liefert die RGBA8-Bytes von Pixel (4,4). */
static int t_proctex_render(ArmCpu *c, unsigned char *mem, float u, float v,
                            uint32_t cfg, uint32_t lut, uint32_t off,
                            int noise_lut_full, unsigned char out[4])
{
    uint32_t *q, size, o;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, 0.0f, u, v, u, v);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x80, 1u << 10);               /* texture3_enable */
    q = emit(q, 0xA9, 0x00000FFFu);            /* Amplitude u/v */
    q = emit(q, 0xAA, 0x00000FFFu);
    q = emit(q, 0xAB, 0x38003800u);            /* Frequenz u/v = 0.5 */
    q = emit(q, 0xA8, cfg);
    q = emit(q, 0xAC, lut);
    q = emit(q, 0xAD, off);
    q = t_emit_proctex_ramps(q, noise_lut_full);
    q = emit_tev0(q, 6u, 6u, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    if (size > LIST2 - LIST) return 0;
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    out[0] = mem[o + 3];
    out[1] = mem[o + 2];
    out[2] = mem[o + 1];
    out[3] = mem[o + 0];
    return 1;
}

static void test_proctex_combiner(ArmCpu *c, unsigned char *mem)
{
    static const struct { uint32_t comb; float u, v; int red; const char *name; } cases[] = {
        {0u, 0.50f, 0.50f, 64,  "U"},
        {1u, 0.50f, 0.50f, 32,  "U2"},
        {2u, 0.75f, 0.25f, 32,  "V"},
        {3u, 0.50f, 0.25f, 8,   "V2"},
        {4u, 0.25f, 0.75f, 64,  "Add"},
        {5u, 0.25f, 0.75f, 40,  "Add2"},
        {6u, 0.25f, 0.75f, 100, "SqrtAdd2"},
        {7u, 0.25f, 0.75f, 32,  "Min"},
        {8u, 0.25f, 0.75f, 95,  "Max"},
        {9u, 0.25f, 0.75f, 81,  "RMax"},
    };
    int i;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        unsigned char px[4];
        uint32_t cfg = 9u | (cases[i].comb << 6);   /* clamp ToEdge, Combiner */
        char msg[96];
        int ok = t_proctex_render(c, mem, cases[i].u, cases[i].v, cfg,
                                  (128u << 11) | 0u, 0u, 0, px);
        sprintf(msg, "Proctex-Combiner %s => Rot %d", cases[i].name, cases[i].red);
        check(ok && px[0] == (unsigned char)cases[i].red && px[3] == 255, msg);
    }
}

static void test_proctex_clamp_shift(ArmCpu *c, unsigned char *mem)
{
    static const struct { uint32_t clamp; float u, v; int red; const char *name; } cases[] = {
        {0u, 2.5f, 0.5f, 0,   "ToZero"},
        {1u, 2.5f, 0.5f, 126, "ToEdge"},
        {2u, 2.5f, 0.5f, 64,  "SymmetricalRepeat"},
        {3u, 2.5f, 0.5f, 64,  "MirroredRepeat"},
        {4u, 2.5f, 0.5f, 126, "Pulse"},
        {5u, 2.5f, 0.5f, 126, "unbekannt => ToEdge"},
        {0u, -2.5f, 0.5f, 0, "ToZero mit abs(u)"},
    };
    int i;
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        unsigned char px[4];
        uint32_t cfg = cases[i].clamp | (1u << 3);   /* v_clamp ToEdge */
        char msg[96];
        int ok = t_proctex_render(c, mem, cases[i].u, cases[i].v, cfg,
                                  (128u << 11), 0u, 0, px);
        sprintf(msg, "Proctex-Clamp %s => Rot %d", cases[i].name, cases[i].red);
        check(ok && px[0] == (unsigned char)cases[i].red, msg);
    }

    /* Shift: u_shift=Odd, v=2.6 => +0.5 auf u (0.25 -> 0.75 => 95).
       v_shift=Even mit u=1.6 => +0.5 auf v (0.25 -> 0.75 => Combiner V 95).
       u_clamp MirroredRepeat verdoppelt den Shift-Offset auf 1.0;
       1.25 wird gespiegelt zu 0.75. */
    {
        unsigned char px[4];
        int ok;

        ok = t_proctex_render(c, mem, 0.25f, 2.6f, 9u | (1u << 16),
                              (128u << 11), 0u, 0, px);
        check(ok && px[0] == 95, "Proctex-Shift u=Odd (v=2.6) => u 0.75, Rot 95");

        ok = t_proctex_render(c, mem, 0.25f, 2.6f,
                              3u | (1u << 3) | (1u << 16), (128u << 11), 0u, 0, px);
        check(ok && px[0] == 95,
              "Proctex-Shift Odd + MirroredRepeat: Offset 1.0, 1.25 -> 0.75");

        ok = t_proctex_render(c, mem, 1.6f, 0.25f,
                              9u | (2u << 6) | (2u << 18), (128u << 11), 0u, 0, px);
        check(ok && px[0] == 95, "Proctex-Shift v=Even (u=1.6) => v 0.75, Rot 95");
    }
}

static void test_proctex_noise(ArmCpu *c, unsigned char *mem)
{
    /* u=0.3, v=0.4, Frequenz je 0.5 (f16 0x3800), Phase 0, Amplitude
       4095 (=-1.0). NoiseCoef liefert -0.69 bei leerer Noise-LUT bzw.
       -0.85 bei LUT=4095; danach abs() und Combiner U => Rot 48 bzw. 69. */
    unsigned char px[4];
    int ok;
    uint32_t cfg = 9u | (1u << 15);
    uint32_t lut = (128u << 11);
    uint32_t *q, size, o;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, 0.0f, 0.3f, 0.4f, 0.3f, 0.4f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x80, 1u << 10);
    q = emit(q, 0xA8, cfg);
    q = emit(q, 0xA9, 0x00000FFFu);
    q = emit(q, 0xAA, 0x00000FFFu);
    q = emit(q, 0xAB, 0x38003800u);
    q = emit(q, 0xAC, lut);
    q = emit(q, 0xAD, 0u);
    q = t_emit_proctex_ramps(q, 0);
    q = emit_tev0(q, 6u, 6u, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 49,
          "Proctex-Rauschen (leere Noise-LUT): -0.69, Rot 49");

    ok = t_proctex_render(c, mem, 0.3f, 0.4f, cfg, lut, 0u, 1, px);
    check(ok && px[0] == 69,
          "Proctex-Rauschen (Noise-LUT=4095): -0.85, Rot 69");
}

static void test_proctex_alpha_filter(ArmCpu *c, unsigned char *mem)
{
    unsigned char px[4];
    int ok;

    /* separate_alpha: Alpha aus Alpha-Map mit Combiner V, v=0.25 =>
       value 1024/4095 * 255 = 63.76 => 63. Farbe bleibt Rot 64. */
    ok = t_proctex_render(c, mem, 0.5f, 0.25f, 9u | (2u << 10) | (1u << 14),
                          (128u << 11), 0u, 0, px);
    check(ok && px[0] == 64 && px[3] == 63,
          "Proctex separate_alpha: Alpha-Map V => 63, Farbe 64");

    /* Filter: Nearest rundet den Index (64), Linear schneidet ab (63).
       Mipmap-Varianten verhalten sich wie in Azahar (TODO mipmap). */
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 0u, 0u, 0, px);
    check(ok && px[0] == 64, "Proctex-Filter Nearest: Index 63.5 => 64");
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 1u, 0u, 0, px);
    check(ok && px[0] == 63, "Proctex-Filter Linear: 63.5 => 63");

    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 2u, 0u, 0, px);
    check(ok && px[0] == 64, "Proctex NearestMipmapNearest = Nearest (64)");
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 3u, 0u, 0, px);
    check(ok && px[0] == 63, "Proctex LinearMipmapNearest = Linear (63)");
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 4u, 0u, 0, px);
    check(ok && px[0] == 64, "Proctex NearestMipmapLinear = Nearest (64)");
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (128u << 11) | 5u, 0u, 0, px);
    check(ok && px[0] == 63, "Proctex LinearMipmapLinear = Linear (63)");

    /* lut_offset level0 = 100, width = 20: index = 100 + 0.500122*19
       = 109.502 => Nearest 110, Linear 109. */
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (20u << 11), 100u, 0, px);
    check(ok && px[0] == 110,
          "Proctex lut_offset 100 + width 20 (Nearest) => 110");
    ok = t_proctex_render(c, mem, 0.5f, 0.5f, 9u, (20u << 11) | 1u, 100u, 0, px);
    check(ok && px[0] == 109, "Proctex lut_offset 100 + width 20 (Linear) => 109");
}

/* LUT-Upload: Index laeuft weiter, 128er-Tabellen modulo 128, 256er
   modulo 256 (Azahar PicaCore::ProcTex). */
static void test_proctex_lut_upload(ArmCpu *c, unsigned char *mem)
{
    uint32_t buf[256];
    uint32_t *q, size, o;
    int i, ok;

    /* Noise-Wertetabelle: 0xAF=127 + ein Wort => Index 0.
       Farbtabelle: Start 255 + 4 Woerter => Index (255+4)&0xFF = 3. */
    gpu_pica_reset();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0xAF, (0u << 8) | 127u);
    q = emit(q, 0xB0, 0x0FFFu);
    buf[0] = 0x00FF0000u;               /* Index 255: blau */
    buf[1] = 0xFF00FF00u;               /* Index 0: gruen */
    buf[2] = 0xFF0000FFu;               /* Index 1: rot */
    buf[3] = 0xFF000000u;               /* Index 2: schwarz */
    q = t_emit_proctex_lut(q, 4u, 255u, buf, 4);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(gpu_pica_reg(0xAF) == 0x403u,
          "Proctex-LUT: 0xAF-Index inkrementiert (255+4 => 3)");

    /* 200 Woerter auf die 128er-Noise-Tabelle: Registerindex 200. */
    gpu_pica_reset();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0xAF, (0u << 8) | 0u);
    for (i = 0; i < 200; i++) q = emit(q, 0xB0, (uint32_t)i);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(gpu_pica_reg(0xAF) == 200u,
          "Proctex-LUT: 200 Woerter => Index 200 (8-Bit-Inkrement)");

    /* Farbtabellen-Umschlag rendern: color_map 0 => lut_coord 0,
       Farbindex 0 wurde bei Index 256 (nach 255 ab Start 255) geschrieben. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x80, 1u << 10);
    q = emit(q, 0xA8, 9u);
    q = emit(q, 0xAC, (1u << 11));             /* width 1 => Index = 0 */
    q = emit(q, 0xAD, 0u);
    memset(buf, 0, sizeof(buf));
    q = t_emit_proctex_lut(q, 2u, 0u, buf, 128);   /* color_map alles 0 */
    q = t_emit_proctex_lut(q, 4u, 0u, buf, 255);   /* Farbtabelle 0..254 = 0 */
    buf[0] = 0xFF000000u;                          /* Index 255: schwarz */
    buf[1] = 0xFF00FF00u;                          /* 256 -> 0: gruen */
    q = t_emit_proctex_lut(q, 4u, 255u, buf, 2);
    q = emit_tev0(q, 6u, 6u, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    ok = (mem[o + 3] == 0 && mem[o + 2] == 255 && mem[o + 1] == 0);
    check(ok, "Proctex-Farbtabelle: Index 256 faellt auf 0 (gruen)");
}

/* texture3_coordinates (0x80 Bits 8..9) waehlt unter TEXCOORD0/1/2.
   Eigenes Vertexprogramm: o1 -> TEXCOORD0 (12/13), o2 -> TEXCOORD1
   (14/15). tc0=(0.5,0.5), tc1=(2.5,2.5); Clamp ToZero macht aus 2.5 die 0. */
static void test_proctex_coordinates(ArmCpu *c, unsigned char *mem)
{
    int sel;
    static const int expect[2] = {64, 0};
    for (sel = 0; sel < 2; sel++) {
        uint32_t words[4];
        uint32_t *q, size, o;
        int i;

        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        for (i = 0; i < 6; i++) {
            static const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1},
                                            {-1, -1}, {1, 1}, {-1, 1}};
            float v[7];
            v[0] = xy[i][0]; v[1] = xy[i][1]; v[2] = 0.0f;
            v[3] = 0.5f; v[4] = 0.5f;              /* TEXCOORD0 */
            v[5] = 2.5f; v[6] = 2.5f;              /* TEXCOORD1 */
            memcpy(c->mem + ((VTX + (uint32_t)i * 28u) & 0x1FFFFFFFu), v, sizeof(v));
        }
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x200, VTX >> 3);
        q = emit(q, 0x201, 0xB | (7u << 4) | (7u << 8));   /* f3, f2, f2 */
        q = emit(q, 0x202, 2u << 28);
        q = emit(q, 0x203, 0u);                 /* Loader 0: attr0/attr1 */
        q = emit(q, 0x204, 0x10u);
        q = emit(q, 0x205, (2u << 28) | (28u << 16));
        q = emit(q, 0x206, 20u);                /* Loader 1: attr2 */
        q = emit(q, 0x207, 0x2u);
        q = emit(q, 0x208, (1u << 28) | (28u << 16));
        q = emit(q, 0x4F, 3u);                  /* 3 Ausgaenge */
        q = emit(q, 0x50, 0x03020100u);         /* o0 -> POSITION */
        q = emit(q, 0x51, 0x1F1F0D0Cu);         /* o1 -> TEXCOORD0 */
        q = emit(q, 0x52, 0x1F1F0F0Eu);         /* o2 -> TEXCOORD1 */
        q = emit(q, 0x2BD, 7u);
        q = emit(q, 0x2B9, 2u);
        q = emit(q, 0x2BB, 0x210u);
        words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);
        words[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);
        words[2] = ins_common(0x13u, 0u, 0x02u, 0u, 2u, 0u);
        words[3] = 0x22u << 26;
        q = emit_program(q, words, 4);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit_viewport(q, 8, 8);
        q = emit(q, 0x228, 6u);
        q = emit(q, 0x80, (1u << 10) | ((uint32_t)sel << 8));
        q = emit(q, 0xA8, 8u);
        q = emit(q, 0xAC, (128u << 11));
        q = emit(q, 0xAD, 0u);
        q = t_emit_proctex_ramps(q, 0);
        q = emit_tev0(q, 6u, 6u, 0u);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
        {
            char msg[96];
            sprintf(msg, "Proctex texture3_coordinates=%d => Rot %d", sel, expect[sel]);
            check(mem[o + 3] == (unsigned char)expect[sel], msg);
        }
    }
}

static void test_fb_formats(ArmCpu *c, unsigned char *mem)
{
    static const struct {
        uint32_t fmt; int bpp; const char *name;
        unsigned char col[4]; unsigned char bytes[4];
    } cases[] = {
        {0, 4, "RGBA8",  {0x12, 0x34, 0x56, 0x78}, {0x78, 0x56, 0x34, 0x12}},
        {1, 3, "RGB8",   {0x12, 0x34, 0x56, 0xFF}, {0x56, 0x34, 0x12, 0x00}},
        {2, 2, "RGB5A1", {0xF8, 0x08, 0x10, 0xFF}, {0x45, 0xF8, 0x00, 0x00}},
        {3, 2, "RGB565", {0xF8, 0xFC, 0x10, 0xFF}, {0xE2, 0xFF, 0x00, 0x00}},
        {4, 2, "RGBA4",  {0xFF, 0x22, 0x33, 0x44}, {0x34, 0xF2, 0x00, 0x00}},
    };
    int i;
    for (i = 0; i < 5; i++) {
        uint32_t *q, size, o;
        uint32_t cst = (uint32_t)cases[i].col[0] | ((uint32_t)cases[i].col[1] << 8) |
                       ((uint32_t)cases[i].col[2] << 16) | ((uint32_t)cases[i].col[3] << 24);
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x200);
        t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_fb(q, 0x40000u, 8, 8, cases[i].fmt);
        q = emit_tev0(q, 0xEu, 0xEu, cst);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 2, 8, 8, cases[i].bpp);
        {
            char msg[96];
            int k, ok = 1;
            for (k = 0; k < cases[i].bpp; k++)
                if (mem[o + k] != cases[i].bytes[k]) ok = 0;
            sprintf(msg, "Farbziel-Format %s kodiert (Azahar Encode%s)", cases[i].name,
                    cases[i].name);
            check(ok, msg);
        }
    }

}

static void test_logic_ops(ArmCpu *c, unsigned char *mem)
{
    static const unsigned s = 0x3C, d = 0x0F;
    uint32_t op;
    for (op = 0; op < 16u; op++) {
        uint32_t *q, size, o;
        unsigned e, r0;
        switch (op) {
        case 0: e = 0; break;
        case 1: e = s & d; break;
        case 2: e = s & (~d & 0xFFu); break;
        case 3: e = s; break;
        case 4: e = 0xFF; break;
        case 5: e = (~s) & 0xFFu; break;
        case 6: e = d; break;
        case 7: e = (~d) & 0xFFu; break;
        case 8: e = (~(s & d)) & 0xFFu; break;
        case 9: e = (s | d) & 0xFFu; break;
        case 10: e = (~(s | d)) & 0xFFu; break;
        case 11: e = (s ^ d) & 0xFFu; break;
        case 12: e = (~(s ^ d)) & 0xFFu; break;
        case 13: e = ((~s) & d) & 0xFFu; break;
        case 14: e = (s | (~d)) & 0xFFu; break;
        default: e = ((~s) | d) & 0xFFu; break;
        }
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
        mem[o + 0] = (unsigned char)d;
        mem[o + 1] = (unsigned char)d;
        mem[o + 2] = (unsigned char)d;
        mem[o + 3] = (unsigned char)d;
        t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit(q, 0x102, op);                /* LogicOp statt Blending */
        q = emit_tev0(q, 0xEu, 0xEu, 0x3C3C3C3Cu);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        r0 = mem[o + 3];
        {
            char msg[80];
            sprintf(msg, "LogicOp 0x%X: src 0x3C, dest 0x0F => 0x%02X", op, e);
            check(r0 == e, msg);
        }
    }
}

static void test_alpha_test(ArmCpu *c, unsigned char *mem)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        uint32_t *q, size, o;
        uint32_t at = 1u | (pass ? (7u << 4) : (4u << 4)) | (0x40u << 8);
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad(q, VTX, 8, 8);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit(q, 0x104, at);                /* alpha_test enable/func/ref */
        q = emit_tev0(q, 0xEu, 0xEu, 0x40000000u);   /* alpha 0x40 */
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
 
        check(mem[o + 0] == (pass ? 0x40 : 0x00),
              pass ? "Alpha-Test GE 0x40 mit alpha 0x40 => geschrieben"
                   : "Alpha-Test L 0x40 mit alpha 0x40 => verworfen");
    }
}

static void test_depth_stencil(ArmCpu *c, unsigned char *mem)
{
    uint32_t *q, size, o, dpo;
    /* 1) Tiefe schreiben: z=0.5, DepthWrite ohne Test. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(mem + 0x44000u, 0, 0x200);
    t_quad_vertices(c, VTX, -0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x116, 3u);                    /* D24S8, RGBA8 */
    q = emit(q, 0x4D, f24raw(-1.0f));           /* viewport depth range */
    q = emit(q, 0x4E, f24raw(0.0f));           /* viewport depth near */
    q = emit(q, 0x11C, 0x44000u >> 3);
    q = emit(q, 0x115, 1u);                    /* allow_depth_stencil_write */
    q = emit(q, 0x11D, 0x40000u >> 3);
    q = emit(q, 0x11E, 8u | (7u << 12));
    q = emit(q, 0x107, 0x00000F00u | (1u << 12));   /* RGBA + depth write */
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);     /* rot */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    dpo = t_fb_off(0x44000u, 1, 2, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[dpo + 0] == 0xFF && mem[dpo + 1] == 0xFF &&
          mem[dpo + 2] == 0x7F && mem[dpo + 3] == 0,
          "Depth-Write: z=0.5 => 0x7FFFFF im D24S8-Puffer");

    /* 2) Weiter weg (z=0.75) mit LessThan: verworfen. */
    gpu_pica_reset();
    t_quad_vertices(c, VTX, -0.75f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x116, 3u);
    q = emit(q, 0x4D, f24raw(-1.0f));
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0x11C, 0x44000u >> 3);
    q = emit(q, 0x115, 1u);
    q = emit(q, 0x11D, 0x40000u >> 3);
    q = emit(q, 0x11E, 8u | (7u << 12));
    q = emit(q, 0x107, 0x00000F00u | (4u << 4) | 1u | (1u << 12));
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF00FF00u);     /* gruen */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[dpo + 2] == 0x7F,
          "Depth-Test LessThan: z=0.75 verworfen, Farbe/Tiefe unveraendert");

    /* 3) Naeher (z=0.25): angenommen. */
    gpu_pica_reset();
    t_quad_vertices(c, VTX, -0.25f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x116, 3u);
    q = emit(q, 0x4D, f24raw(-1.0f));
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0x11C, 0x44000u >> 3);
    q = emit(q, 0x115, 1u);
    q = emit(q, 0x11D, 0x40000u >> 3);
    q = emit(q, 0x11E, 8u | (7u << 12));
    q = emit(q, 0x107, 0x00000F00u | (4u << 4) | 1u | (1u << 12));
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF00FF00u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 3] == 0 && mem[o + 2] == 0xFF && mem[dpo + 2] == 0x3F,
          "Depth-Test LessThan: z=0.25 angenommen (Farbe+Tiefe neu)");

    /* 4) Stencil: Equal ref=1 mit depth_pass=Zero, dann zweiter Draw. */
    mem[dpo + 3] = 1;                          /* Stencil vorbelegen */
    gpu_pica_reset();
    t_quad_vertices(c, VTX, -0.1f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x116, 3u);
    q = emit(q, 0x4D, f24raw(-1.0f));
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0x11C, 0x44000u >> 3);
    q = emit(q, 0x115, 1u);
    q = emit(q, 0x11D, 0x40000u >> 3);
    q = emit(q, 0x11E, 8u | (7u << 12));
    q = emit(q, 0x107, 0x00000F00u);           /* kein Depth-Test/Write */
    q = emit(q, 0x105, 1u | (2u << 4) | (0xFFu << 8) | (1u << 16) | (0xFFu << 24));
    q = emit(q, 0x106, 0u | (0u << 4) | (1u << 8));
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFFFF0000u);     /* blau */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[dpo + 3] == 0 && mem[o + 1] == 0xFF,
          "Stencil Equal/Replace: Durchlauf setzt Stencil 0 und schreibt");

    /* Zweiter Draw: Stencil 0 != ref 1 => kein Schreiben. */
    gpu_pica_reset();
    t_quad_vertices(c, VTX, -0.1f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x116, 3u);
    q = emit(q, 0x4D, f24raw(-1.0f));
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0x11C, 0x44000u >> 3);
    q = emit(q, 0x115, 1u);
    q = emit(q, 0x11D, 0x40000u >> 3);
    q = emit(q, 0x11E, 8u | (7u << 12));
    q = emit(q, 0x107, 0x00000F00u);
    q = emit(q, 0x105, 1u | (2u << 4) | (0xFFu << 8) | (1u << 16) | (0xFFu << 24));
    q = emit(q, 0x106, 0u | (0u << 4) | (1u << 8));
    q = emit(q, 0x102, 3u);
    q = emit(q, 0x113, 1u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF00FF00u);     /* gruen */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 1] == 0xFF && mem[o + 2] == 0 && mem[dpo + 3] == 0,
          "Stencil-Test schlaegt fehl => Farbe/Stencil unveraendert");
}

static void test_texture_type_and_tc2(ArmCpu *c, unsigned char *mem)
{
    /* Azahar pica/output_vertex.cpp: TEXCOORD2 sind Semantic 22/23; 16 ist
       TEXCOORD0_W. sw_rasterizer.cpp TextureColor: nur Textureinheit 0
       beachtet den Typ (Bits28..30), Einheit 1/2 ignorieren ihn. */
    static const unsigned char red[4] = {0xFF, 0x00, 0x00, 0xFF};
    static const unsigned char green[4] = {0x00, 0xFF, 0x00, 0xFF};
    unsigned char *tile = mem + TEX;
    uint32_t *q, size, o, words[3];
    int v;

    /* 1) Einheit 2 sampelt TEXCOORD2 (22/23): Vertex-Shader legt attr2 nach
          o1 mit Semantik 22/23. Textur rot, nur Texel (7,0) gruen. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(tile, 0, 0x100);
    for (v = 0; v < 64; v++) t_tex_encode(tile, 0, (unsigned)v, red);
    t_tex_encode(tile, 0, (unsigned)t_morton(7, 0), green);
    {
        static const float p[3][2] = {{-1.0f, -1.0f}, {3.0f, -1.0f}, {-1.0f, 3.0f}};
        for (v = 0; v < 3; v++) {
            float f[12];
            memset(f, 0, sizeof(f));
            f[0] = p[v][0]; f[1] = p[v][1]; f[2] = 0.0f; f[3] = 1.0f;
            f[4] = 0.0f; f[5] = 0.0f; f[6] = 0.0f; f[7] = 1.0f;
            f[8] = 0.5f; f[9] = 0.5f; f[10] = 0.0f; f[11] = 1.0f;
            memcpy(mem + ((VTX3 + (uint32_t)v * 48u) & 0x1FFFFFFFu), f, sizeof(f));
        }
    }
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x200, VTX3 >> 3);
    q = emit(q, 0x201, 0xFFFu);                /* attr0..2: float, 4 Komp. */
    q = emit(q, 0x202, 2u << 28);              /* 3 Attribute */
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x0210u);               /* comps: attr0, attr1, attr2 */
    q = emit(q, 0x205, (3u << 28) | (48u << 16));
    q = emit(q, 0x2B9, 2u);
    q = emit(q, 0x2BB, 0x210u);                /* attr0->v0, attr1->v1, attr2->v2 */
    q = emit(q, 0x4F, 2u);
    q = emit(q, 0x50, 0x03020100u);            /* o0 = POSITION */
    q = emit(q, 0x51, 0x1F1F1716u);            /* o1 = TEXCOORD2 (22/23) */
    q = emit(q, 0x2BD, 3u);
    words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);   /* mov o0,v0 */
    words[1] = ins_common(0x13u, 0u, 0x02u, 0u, 1u, 0u);   /* mov o1,v2 */
    words[2] = 0x22u << 26;
    q = emit_program(q, words, 3);
    q = emit_swizzle0(q, swz_xyzw());
    q = emit(q, 0x80, 1u << 2);                /* texture2_enable */
    q = emit(q, 0x9A, (8u << 16) | 8u);
    q = emit(q, 0x9B, 0u);
    q = emit(q, 0x9D, TEX >> 3);
    q = emit(q, 0x9E, 0u);                     /* RGBA8 */
    q = emit_viewport(q, 8, 8);
    q = emit(q, 0x228, 3u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 5u, 5u, 0u);              /* Quelle Texture2 */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0x00 && mem[o + 1] == 0x00,
          "Texture2 nutzt TEXCOORD2 22/23 (0.5,0.5) => rot");

    /* 2) Typ 5 (Disabled) auf Einheit 1 wird ignoriert (nur Einheit 0). */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(tile, 0, 0x100);
    for (v = 0; v < 64; v++) t_tex_encode(tile, 0, (unsigned)v, red);
    t_quad_vertices(c, VTX, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x80, 1u << 1);                /* texture1_enable */
    q = emit(q, 0x92, (8u << 16) | 8u);
    q = emit(q, 0x93, 5u << 28);               /* type = Disabled */
    q = emit(q, 0x95, TEX >> 3);
    q = emit(q, 0x96, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 4u, 4u, 0u);              /* Quelle Texture1 */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0x00 && mem[o + 1] == 0x00,
          "Texture1 ignoriert Typ Disabled (3dbrew: nur Einheit 0)");

    /* 3) Einheit 0 mit Typ Disabled liefert (0,0,0,0). */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = emit(q, 0x80, 1u);                     /* texture0_enable */
    q = emit(q, 0x82, (8u << 16) | 8u);
    q = emit(q, 0x83, 5u << 28);               /* type = Disabled */
    q = emit(q, 0x85, TEX >> 3);
    q = emit(q, 0x8E, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);              /* Quelle Texture0 */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    check(mem[o + 3] == 0 && mem[o + 2] == 0 && mem[o + 1] == 0 && mem[o + 0] == 0,
          "Texture0 mit Typ Disabled => (0,0,0,0)");
}

static void test_fog(ArmCpu *c, unsigned char *mem)
{
    uint32_t *q, size, o;
    /* Azahar sw_rasterizer.cpp WriteFog + pica_core.cpp HandleSpecialReg:
     * Fog-LUT-Eintrag = Bits13..23 value/2047, Bits0..12 signed diff/2047.
     * fog_index = (flip ? 1-depth : depth) * 128; Index-LUT + Diff*Fraktion.
     * Fog-Farbe 0xE1 r/g/b = (0x10,0x20,0x30); Quelle Rot (255,0,0,255). */

    /* 1) Wert 1024/2047 bei Index 64 (depth 0.5, Faktor ~0.5):
       0xE6=64, 0xE8 schreibt genau dorthin, danach steht der Offset auf 65. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, -0.5f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x4D, f24raw(-1.0f));          /* depth = -z */
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0xE6, 64u);
    q = emit(q, 0xE8, 1024u << 13);
    q = emit(q, 0xE1, 0x00302010u);
    q = emit(q, 0xE0, 5u);                     /* fog_mode = Fog */
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    check(gpu_pica_reg(0xE6) == 65u,
          "Fog-LUT: 0xE8 schreibt bei 0xE6 und inkrementiert den Offset");
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0x17 && mem[o + 2] == 0x0F &&
          mem[o + 3] == 0x87,
          "Fog-Faktor 1024/2047 mischt Quelle mit Fog-Farbe (~135,15,23)");

    /* 2) fog_flip: LUT[96] = Faktor 1.0. Depth 0.25 ohne Flip -> Index 32
       (LUT 0 => volle Fog-Farbe); mit Flip -> Index 96 (Quelle unveraendert). */
    {
        int flip;
        for (flip = 0; flip < 2; flip++) {
            gpu_pica_reset();
            memset(mem + 0x40000u, 0, 0x100);
            t_quad_vertices(c, VTX, -0.25f, 0.0f, 0.0f, 1.0f, 1.0f);
            q = (uint32_t *)(mem + LIST);
            q = t_emit_quad(q, VTX, 8, 8);
            q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
            q = emit(q, 0x4D, f24raw(-1.0f));
            q = emit(q, 0x4E, f24raw(0.0f));
            q = emit(q, 0xE6, 96u);
            q = emit(q, 0xE8, 2047u << 13);
            q = emit(q, 0xE1, 0x00302010u);
            q = emit(q, 0xE0, 5u | ((uint32_t)flip << 16));
            q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
            q = emit(q, 0x22E, 0u);
            size = (uint32_t)((unsigned char *)q - (mem + LIST));
            gpu_run_pica_list(c, LIST, size);
            o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
            check(flip ? (mem[o + 3] == 0xFF && mem[o + 2] == 0x00 &&
                         mem[o + 1] == 0x00)
                       : (mem[o + 3] == 0x10 && mem[o + 2] == 0x20 &&
                          mem[o + 1] == 0x30),
                  flip ? "fog_flip: Index 96 => Quelle bleibt (Faktor 1)"
                       : "fog_flip: Index 32 ohne Eintrag => volle Fog-Farbe");
        }
    }

    /* 3) Fraktion: depth = 32.5/128, LUT[32] = value 0, diff 2047 => 0.5. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, -0.25390625f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x4D, f24raw(-1.0f));
    q = emit(q, 0x4E, f24raw(0.0f));
    q = emit(q, 0xE6, 32u);
    q = emit(q, 0xE8, 0x7FFu);                 /* value 0, diff +1.0 */
    q = emit(q, 0xE1, 0x00302010u);
    q = emit(q, 0xE0, 5u);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    check(mem[o + 3] == 0x87 && mem[o + 2] == 0x10 && mem[o + 1] == 0x18,
          "Fog-Fraktion: LUT-Difference * 0.5 => (135,16,24)");
}

static void test_blend(ArmCpu *c, unsigned char *mem)
{
    /* Quelle (128,64,32,192), Ziel (16,32,48,64), Add mit
       SrcAlpha / OneMinusSrcAlpha (Azahar sw_framebuffer.cpp). */
    uint32_t *q, size, o;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x100, 1u << 8);               /* alphablend_enable */
    q = emit(q, 0x101, (0u) | (0u << 8) | (6u << 16) | (7u << 20) | (6u << 24) | (7u << 28));
    q = emit(q, 0x103, 0xFFFFFFFFu);
    q = emit_tev0(q, 0xEu, 0xEu, 0xC0204080u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    o = t_fb_off(0x40000u, 1, 2, 8, 8, 4);
    mem[o + 0] = 64; mem[o + 1] = 48; mem[o + 2] = 32; mem[o + 3] = 16;
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 3] == 100 && mem[o + 2] == 56 && mem[o + 1] == 35 && mem[o + 0] == 160,
          "Blending Add SrcAlpha/1-SrcAlpha => (100,56,35,160)");
}

/* ============ Nachtrag: Register-/Pipeline-Abweichungen ============ */

static void t_pos4(ArmCpu *c, uint32_t off, float x, float y, float z)
{
    float f[4];
    f[0] = x; f[1] = y; f[2] = z; f[3] = 1.0f;
    memcpy(c->mem + ((VTX + off) & 0x1FFFFFFFu), f, sizeof(f));
}

/* Positionsprogramm: attr0 (float4) -> v0 -> o0 -> POSITION. */
static uint32_t *t_emit_vs_pos(uint32_t *q)
{
    uint32_t w[2];
    q = emit(q, 0x2B9, 0u);
    q = emit(q, 0x2BB, 0u);
    q = emit(q, 0x4F, 1u);
    q = emit(q, 0x50, 0x03020100u);
    q = emit(q, 0x2BD, 1u);
    w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);   /* mov o0,v0 */
    w[1] = 0x22u << 26;
    q = emit_program(q, w, 2);
    return emit_swizzle0(q, swz_xyzw());
}

static uint32_t *t_emit_pos_setup(uint32_t *q)
{
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0x0Fu);             /* attr0 float4 */
    q = emit(q, 0x202, 0u);
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0u);
    q = emit(q, 0x205, (1u << 28) | (16u << 16));
    return t_emit_vs_pos(q);
}

static void test_vs_entrypoint(ArmCpu *c, unsigned char *mem)
{
    /* GPUREG_VSH_ENTRYPOINT 0x2BA Bits0..15: Azahar startet den Interpreter
       bei main_offset, nicht bei pc=0. */
    uint32_t words[3], pack[3], *q, size;
    float pos[4];

    gpu_pica_reset();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x4F, 1u);
    q = emit(q, 0x50, 0x03020100u);
    q = emit(q, 0x2BD, 1u);
    q = emit(q, 0x2C0, 0u);
    f24_pack3(1.0f, 0.0f, 0.0f, 0.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    f24_pack3(2.0f, 2.0f, 2.0f, 2.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    words[0] = ins_common(0x13u, 0u, 0x20u, 0u, 0u, 0u);   /* mov o0,c0 */
    words[1] = ins_common(0x13u, 0u, 0x21u, 0u, 0u, 0u);   /* mov o0,c1 */
    words[2] = 0x22u << 26;
    q = emit_program(q, words, 3);
    q = emit_swizzle0(q, swz_xyzw());
    q = emit(q, 0x2BA, 1u);                /* Einstieg bei Wort 1 */
    q = emit(q, 0x228, 1u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    gpu_pica_last_vertex(pos, NULL, NULL);
    check(pos[0] == 2.0f && pos[1] == 2.0f && pos[2] == 2.0f && pos[3] == 2.0f,
          "VS-Entrypoint 0x2BA=1 ueberspringt Wort 0 (c1 statt c0)");
}

static void test_vs_dphi(ArmCpu *c, unsigned char *mem)
{
    /* DPHI (Opcode 0x18) ist SrcInversed (nihstro:
       TwoArguments | SrcInversed): src1i = Bits14..18 (5 Bit),
       src2i = Bits7..13 (7 Bit). Hier src1i = v0 (0x00),
       src2i = c0 (0x20). DPH/DPHI setzt src1.w = 1.
       v0=(5,6,7,8), c0=(1,2,3,4) => 5*1+6*2+7*3+1*4 = 42.
       Ein nicht-invertiertes Dekodieren (src1=(instr>>12)&0x7F,
       src2=(instr>>7)&0x1F) liest stattdessen v1 (nicht gemappt, also 0)
       und v0 und ergibt 1*8 = 8. */
    uint32_t words[2], pack[3], *q, size;
    float pos[4], expect;

    gpu_pica_reset();
    t_pos4(c, 0, 5.0f, 6.0f, 7.0f);
    ((float *)(mem + ((VTX & 0x1FFFFFFFu))))[3] = 8.0f;
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit(q, 0x2C0, 0u);
    f24_pack3(1.0f, 2.0f, 3.0f, 4.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    words[0] = ins_common_inv(0x18u, 0u, 0x00u, 0x20u, 0u, 0u); /* dphi o0,v0,c0 */
    words[1] = 0x22u << 26;
    q = emit_program(q, words, 2);
    q = emit_swizzle0(q, swz_xyzw());
    q = emit(q, 0x228, 1u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    gpu_pica_last_vertex(pos, NULL, NULL);
    expect = 5.0f * 1.0f + 6.0f * 2.0f + 7.0f * 3.0f + 1.0f * 4.0f;
    check(pos[0] == expect && pos[1] == expect &&
          pos[2] == expect && pos[3] == expect &&
          expect == 42.0f,
          "DPHI 0x18 invertiert: src1i=v0 (5 Bit), src2i=c0 (7 Bit) => 42");
}

static void test_vs_inverted_ops(ArmCpu *c, unsigned char *mem)
{
    /* SGEI 0x1A und SLTI 0x1B nutzen dasselbe SrcInversed-Format wie
       DPHI/DSTI (src1i 5 Bit, src2i 7 Bit). Erwartung je Komponente
       unabhaengig aus den Quellwerten berechnet; im nicht-invertierten
       Format waere src1=v1 (0) und src2=v0, das Ergebnis also anders. */
    static const uint32_t ops[2] = {0x1Au, 0x1Bu};
    static const float cvals[2][4] = {{1.0f, 2.0f, 3.0f, 4.0f},
                                      {6.0f, 6.0f, 8.0f, 9.0f}};
    static const float v0[4] = {5.0f, 6.0f, 7.0f, 8.0f};
    int pass, i;

    for (pass = 0; pass < 2; pass++) {
        uint32_t words[2], pack[3], *q, size;
        float pos[4], expect[4];

        gpu_pica_reset();
        t_pos4(c, 0, v0[0], v0[1], v0[2]);
        ((float *)(mem + ((VTX & 0x1FFFFFFFu))))[3] = v0[3];
        q = (uint32_t *)(mem + LIST);
        q = t_emit_pos_setup(q);
        q = emit(q, 0x2C0, 0u);
        f24_pack3(cvals[pass][0], cvals[pass][1], cvals[pass][2],
                  cvals[pass][3], pack);
        q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]);
        q = emit(q, 0x2C1, pack[2]);
        words[0] = ins_common_inv(ops[pass], 0u, 0x00u, 0x20u, 0u, 0u);
        words[1] = 0x22u << 26;
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 1u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        gpu_pica_last_vertex(pos, NULL, NULL);
        for (i = 0; i < 4; i++) {
            int r = (ops[pass] == 0x1Au) ? (v0[i] >= cvals[pass][i])
                                         : (v0[i] < cvals[pass][i]);
            expect[i] = r ? 1.0f : 0.0f;
        }
        check(pos[0] == expect[0] && pos[1] == expect[1] &&
              pos[2] == expect[2] && pos[3] == expect[3],
              pass ? "SLTI 0x1B invertiert: src1i=v0, src2i=c0 => (1,0,1,1)"
                   : "SGEI 0x1A invertiert: src1i=v0, src2i=c0 => (1,1,1,1)");
    }
}

static void test_cull_keep_clockwise(ArmCpu *c, unsigned char *mem)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        uint32_t *q, size, o;
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        if (pass) {   /* Bildschirm-SignedArea > 0 */
            t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
            t_pos4(c, 16, 1.0f, -1.0f, 0.0f);
            t_pos4(c, 32, -1.0f, 1.0f, 0.0f);
        } else {      /* Bildschirm-SignedArea < 0 */
            t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
            t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
            t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
        }
        q = (uint32_t *)(mem + LIST);
        q = t_emit_pos_setup(q);
        q = emit_viewport(q, 8, 8);
        q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit(q, 0x40, 1u);                 /* KeepClockWise */
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
        check(pass ? (mem[o + 3] == 0 && mem[o + 2] == 0 && mem[o + 1] == 0)
                   : (mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0),
              pass ? "Culling KeepClockWise: SignedArea>0 wird verworfen"
                   : "Culling KeepClockWise: SignedArea<0 wird gezeichnet");
    }
}

static void test_triangle_strip(ArmCpu *c, unsigned char *mem)
{
    uint32_t *q, size, o_lo, o_hi;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
    t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
    t_pos4(c, 48, 1.0f, 1.0f, 0.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit_viewport(q, 8, 8);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x40, 1u);                     /* KeepClockWise (A<0 wird gezeichnet) */
    q = emit(q, 0x25E, 1u << 8);               /* Strip */
    q = emit(q, 0x228, 4u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o_lo = t_fb_off(0x40000u, 2, 5, 8, 8, 4);
    o_hi = t_fb_off(0x40000u, 6, 3, 8, 8, 4);
    check(mem[o_lo + 3] == 0xFF && mem[o_hi + 3] == 0xFF,
          "Strip: zweites Dreieck (v2,v1,v3) hat dieselbe Windung => beide sichtbar");

    /* Zustand ueber Draws: erste 2 Vertices, dann die naechsten 2 ohne
       Restart. Ohne erhaltenen Assembler-Zustand entstuende kein Dreieck. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit_viewport(q, 8, 8);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x40, 1u);
    q = emit(q, 0x25E, 1u << 8);
    q = emit(q, 0x228, 2u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x22A, 2u);                    /* vertex_offset */
    q = emit(q, 0x228, 2u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o_lo + 3] == 0xFF && mem[o_hi + 3] == 0xFF,
          "Strip-Zustand bleibt ueber Draws erhalten");

    /* 0x25F restart_primitive verwirft den gepufferten Vertex. */
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x25F, 0u);
    q = emit(q, 0x22A, 2u);
    q = emit(q, 0x228, 2u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o_lo + 3] == 0 && mem[o_hi + 3] == 0,
          "0x25F restart_primitive: zwei Vertices ergeben kein Dreieck");
}

static void test_immediate_topology(ArmCpu *c, unsigned char *mem)
{
    uint32_t *q, size, o_lo, o_hi, pack[3];
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0x0Fu);
    q = emit(q, 0x202, 0u);
    q = t_emit_vs_pos(q);
    q = emit_viewport(q, 8, 8);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x40, 1u);
    q = emit(q, 0x25E, 1u << 8);               /* Strip */
    q = emit(q, 0x242, 0u);                    /* 1 Attribut je Vertex */
    q = emit(q, 0x232, 0xFu);                  /* Immediate-Modus */
    {
        static const float p[4][4] = {
            {-1.0f, -1.0f, 0.0f, 1.0f}, {-1.0f, 1.0f, 0.0f, 1.0f},
            {1.0f, -1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f, 1.0f}
        };
        int i;
        for (i = 0; i < 4; i++) {
            f24_pack3(p[i][0], p[i][1], p[i][2], p[i][3], pack);
            q = emit(q, 0x233, pack[0]);
            q = emit(q, 0x233, pack[1]);
            q = emit(q, 0x233, pack[2]);
        }
    }
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o_lo = t_fb_off(0x40000u, 2, 6, 8, 8, 4);
    o_hi = t_fb_off(0x40000u, 6, 2, 8, 8, 4);
    check(mem[o_lo + 3] == 0xFF && mem[o_hi + 3] == 0xFF,
          "Immediate-Mode nutzt die konfigurierte Strip-Topologie");
}

static void test_etc1(ArmCpu *c, unsigned char *mem)
{
    /* Bekannter ETC1-Block: Individual-Modus, r1=0xF, g/b=0,
       table_subindexes Bit6 gesetzt. ETC1-Subtile = x/4+2*(y/4),
       Modifier-Index = 4*x+y (Azahar etc1.cpp/texture_decode.cpp). */
    static const uint64_t block = 0xF000000000000040ull;
    uint32_t *q, size, o;
    int i;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(mem + TEX, 0, 32);
    for (i = 0; i < 4; i++) memcpy(mem + TEX + i * 8u, &block, 8);
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 12, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);     /* texel (1,2) */
    check(mem[o + 3] == 0xFF && mem[o + 2] == 8 && mem[o + 1] == 8,
          "ETC1 bekannter Block: Texel (1,2) => (255,8,8)");
    o = t_fb_off(0x40000u, 1, 6, 8, 8, 4);     /* texel (1,1) */
    check(mem[o + 3] == 0xFF && mem[o + 2] == 2 && mem[o + 1] == 2,
          "ETC1 Modifier-Index 4*x+y: Texel (1,1) => (255,2,2)");

    /* ETC1A4: Alpha-Nibble bei Shift 4*((x%4)*4+(y%4)); fuer (1,2) also
       Bits24..27 = 7 => 0x77. Ein Morton-Index wuerde hier 0 liefern. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(mem + TEX, 0, 64);
    for (i = 0; i < 4; i++) {
        uint64_t alpha = (uint64_t)7u << 24;   /* lokales (1,2) in jeder Subtile */
        memcpy(mem + TEX + i * 16u, &alpha, 8);
        memcpy(mem + TEX + i * 16u + 8u, &block, 8);
    }
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 13, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 8 && mem[o + 1] == 8 && mem[o + 0] == 0x77,
          "ETC1A4: Alpha-Shift 4*((x%4)*4+(y%4)) => 0x77");
}

static void test_uniform_masks(ArmCpu *c, unsigned char *mem)
{
    /* Azahar HandleSpecialReg liest fuer Bool-/Int-Uniforms den maskierten
       Registerwert. Roher Schreibwert 0x100 bei Maske 0x1 darf b8 nicht
       setzen; roher Wert 2 bei Maske 0x2 darf i0.x nicht setzen. */
    uint32_t words[4], pack[3], *q, size;
    float pos[4];

    /* Bool: b8 = 0 => else-Zweig (c1). */
    gpu_pica_reset();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x4F, 1u);
    q = emit(q, 0x50, 0x03020100u);
    q = emit(q, 0x2BD, 1u);
    q = emit(q, 0x2C0, 0u);
    f24_pack3(1.0f, 0.0f, 0.0f, 0.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    f24_pack3(2.0f, 0.0f, 0.0f, 0.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    words[0] = ins_flow(0x27u, 1u, 2u, 8u, 0u, 0u);       /* ifu b8 */
    words[1] = ins_common(0x13u, 0u, 0x20u, 0u, 0u, 0u);
    words[2] = ins_common(0x13u, 0u, 0x21u, 0u, 0u, 0u);
    words[3] = 0x22u << 26;
    q = emit_program(q, words, 4);
    q = emit_swizzle0(q, swz_xyzw());
    q = emit_h(q, 0x100u, 0x2B0u, 0x1u);   /* roh Bit8, Maske nur Byte0 */
    q = emit(q, 0x228, 1u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    gpu_pica_last_vertex(pos, NULL, NULL);
    check(pos[0] == 2.0f, "Bool-Uniform 0x2B0 nutzt maskierten Registerwert");

    /* Int: i0.x = 0 => LOOP-Rumpf genau einmal => r0 = 1+1 = 2. */
    gpu_pica_reset();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x4F, 1u);
    q = emit(q, 0x50, 0x03020100u);
    q = emit(q, 0x2BD, 1u);
    q = emit(q, 0x2C0, 0u);
    f24_pack3(1.0f, 1.0f, 1.0f, 1.0f, pack);
    q = emit(q, 0x2C1, pack[0]); q = emit(q, 0x2C1, pack[1]); q = emit(q, 0x2C1, pack[2]);
    words[0] = ins_common(0x13u, 0u, 0x20u, 0u, 0x10u, 0u);  /* mov r0,c0 */
    words[1] = ins_flow(0x29u, 0u, 2u, 0u, 0u, 0u);          /* loop i0 */
    words[2] = ins_common(0x00u, 0u, 0x20u, 0x10u, 0x10u, 0u); /* add r0,c0,r0 */
    words[3] = ins_common(0x13u, 0u, 0x10u, 0u, 0u, 0u);    /* mov o0,r0 */
    /* END als fuenftes Wort: emit_program braucht 5 Woerter. */
    {
        uint32_t w5[5];
        int i;
        for (i = 0; i < 4; i++) w5[i] = words[i];
        w5[4] = 0x22u << 26;
        q = emit_program(q, w5, 5);
    }
    q = emit_swizzle0(q, swz_xyzw());
    q = emit_h(q, 0x00000002u, 0x2B1u, 0x2u); /* roh x=2, Maske Byte1 */
    q = emit(q, 0x228, 1u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    gpu_pica_last_vertex(pos, NULL, NULL);
    check(pos[0] == 2.0f, "Int-Uniform 0x2B1 nutzt maskierten Registerwert");
}

static void test_no_program(ArmCpu *c, unsigned char *mem)
{
    unsigned long long before;
    uint32_t *q, size;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, 1.0f, -1.0f, 0.0f);
    t_pos4(c, 32, 0.0f, 1.0f, 0.0f);
    before = gpu_pica_draw_count();
    q = (uint32_t *)(mem + LIST);
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0x0Fu);
    q = emit(q, 0x202, 0u);
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0u);
    q = emit(q, 0x205, (1u << 28) | (16u << 16));
    q = emit_viewport(q, 8, 8);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x228, 3u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(gpu_pica_draw_count() == before &&
          mem[t_fb_off(0x40000u, 4, 4, 8, 8, 4) + 3] == 0,
          "Ohne Vertex-Programm wird nichts gezeichnet (kein Attribut-Fallback)");
}

static void test_fb_clamp(ArmCpu *c, unsigned char *mem)
{
    uint32_t *q, size;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0xAA, 0x3000);
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 16, 16);           /* Viewport groesser als FB */
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);      /* Farbziel nur 8x8 */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[t_fb_off(0x40000u, 1, 1, 8, 8, 4) + 3] == 0xFF,
          "Framebuffer-Clamp: Pixel innerhalb des 8x8-Ziels wird geschrieben");
    check(mem[0x40100u] == 0xAA && mem[0x40200u] == 0xAA,
          "Framebuffer-Clamp: kein Schreiben ausserhalb des Farbziels");
}

/* ================= Fragment-Lighting + Texturtypen (Azahar) =================
 * Erwartungswerte direkt aus tools/azahar:
 *   sw_lighting.cpp ComputeFragmentsColors, regs_lighting.h (0x140..0x1D9)
 *   pica_core.cpp HandleSpecialReg (LUT 0x1C5/0x1C8..0x1CF)
 *   sw_rasterizer.cpp TextureColor + sw_clipper.cpp ConvertCubeCoord
 *   output_vertex.cpp (Semantics 4..7 QUAT, 16 TEXCOORD0_W, 18..20 VIEW)
 */

/* Halb-Float (f16) und f20 (Float<12,7>) fuer Testvektoren. */
static uint32_t f16raw(float v)
{
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x8000u; v = -v; }
    m = frexpf(v, &e);
    return sign | ((uint32_t)(e - 1 + 15) << 10) |
           ((uint32_t)((m * 2.0f - 1.0f) * 1024.0f + 0.5f) & 0x3FFu);
}

static uint32_t f20raw(float v)
{
    int e;
    float m;
    uint32_t sign = 0;
    if (v == 0.0f) return 0;
    if (v < 0.0f) { sign = 0x80000u; v = -v; }
    m = frexpf(v, &e);
    return sign | ((uint32_t)(e - 1 + 63) << 12) |
           ((uint32_t)((m * 2.0f - 1.0f) * 4096.0f + 0.5f) & 0xFFFu);
}

/* Vertexprogramm: attr0 -> POSITION, attr1 -> TEXCOORD0, attr2.x ->
   TEXCOORD0_W, attr2.yzw -> VIEW (18..20), attr3 -> QUATERNION (4..7). */
static uint32_t *t_emit_vs_light(uint32_t *q)
{
    uint32_t w[5];
    q = emit(q, 0x2B9, 3u);                /* max input attribute */
    q = emit(q, 0x2BB, 0x3210u);           /* attr0..3 -> v0..v3 */
    q = emit(q, 0x4F, 4u);                 /* 4 Ausgangsattribute */
    q = emit(q, 0x50, 0x03020100u);        /* o0 -> POSITION */
    q = emit(q, 0x51, 0x1F1F0D0Cu);        /* o1 -> TEXCOORD0 */
    q = emit(q, 0x52, 0x14131210u);        /* o2 -> tc0_w + VIEW */
    q = emit(q, 0x53, 0x07060504u);        /* o3 -> QUATERNION */
    q = emit(q, 0x2BD, 0xFu);
    w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);
    w[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);
    w[2] = ins_common(0x13u, 0u, 0x02u, 0u, 2u, 0u);
    w[3] = ins_common(0x13u, 0u, 0x03u, 0u, 3u, 0u);
    w[4] = 0x22u << 26;
    q = emit_program(q, w, 5);
    return emit_swizzle0(q, swz_xyzw());
}

/* Sechs Quad-Vertices: pos float3, uv float2, [w,viewx,viewy,viewz] float4,
   quat float4; Stride 52. */
static void t_light_vertices(ArmCpu *c, uint32_t vtx, float z,
                             float u0, float v0, float u1, float v1,
                             float tc0_w, const float view[3], const float quat[4])
{
    static const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    int i;
    for (i = 0; i < 6; i++) {
        float f[13];
        f[0] = xy[i][0];
        f[1] = xy[i][1];
        f[2] = z;
        f[3] = u0 + (xy[i][0] + 1.0f) * 0.5f * (u1 - u0);
        f[4] = v0 + (xy[i][1] + 1.0f) * 0.5f * (v1 - v0);
        f[5] = tc0_w;
        f[6] = view[0]; f[7] = view[1]; f[8] = view[2];
        f[9] = quat[0]; f[10] = quat[1]; f[11] = quat[2]; f[12] = quat[3];
        memcpy(c->mem + ((vtx + (uint32_t)i * 52u) & 0x1FFFFFFFu), f, sizeof(f));
    }
}

/* Wie t_light_vertices, aber Vertex 0 nutzt q0 und alle anderen q1.
   Fuer den FlipQuaternionIfOpposite-Test (Azahar AddTriangle). */
static void t_light_vertices_q2(ArmCpu *c, uint32_t vtx, float z,
                                float tc0_w, const float view[3],
                                const float q0[4], const float q1[4])
{
    static const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    int i;
    for (i = 0; i < 6; i++) {
        float f[13];
        const float *qq = (i == 0) ? q0 : q1;
        f[0] = xy[i][0];
        f[1] = xy[i][1];
        f[2] = z;
        f[3] = 0.5f;
        f[4] = 0.5f;
        f[5] = tc0_w;
        f[6] = view[0]; f[7] = view[1]; f[8] = view[2];
        f[9] = qq[0]; f[10] = qq[1]; f[11] = qq[2]; f[12] = qq[3];
        memcpy(c->mem + ((vtx + (uint32_t)i * 52u) & 0x1FFFFFFFu), f, sizeof(f));
    }
}

static uint32_t *t_emit_quad_light(uint32_t *q, uint32_t vtx, int vw, int vh)
{
    q = emit(q, 0x200, vtx >> 3);
    q = emit(q, 0x201, 0xFF7Bu);           /* 0:float3, 1:float2, 2:float4, 3:float4 */
    q = emit(q, 0x202, 3u << 28);          /* max_attribute_index = 3 */
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x3210u);           /* comps: attr0..3 */
    q = emit(q, 0x205, (4u << 28) | (52u << 16));
    q = emit(q, 0x228, 6u);
    q = t_emit_vs_light(q);
    return emit_viewport(q, vw, vh);
}

static void test_texcache_etc1(ArmCpu *c, unsigned char *mem)
{
    static const uint64_t block_red = 0xF000000000000040ull;
    static const uint64_t block_zero = 0x0000000000000040ull;
    uint32_t *q, size, o;
    int i;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(mem + TEX, 0, 64);
    for (i = 0; i < 4; i++) {
        uint64_t alpha = (uint64_t)7u << 24;
        memcpy(mem + TEX + i * 16u, &alpha, 8);
        memcpy(mem + TEX + i * 16u + 8u, &block_red, 8);
    }
    t_quad_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 13, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 8 && mem[o + 1] == 8,
          "Textur-Cache: ETC1A4 erster Draw rot (r1=0xF)");

    /* Quellkachel im Gastspielspeicher aendern (wie ein Upload): der
       Kachel-Cache muss die Dekodierung verwerfen. r1 0xF -> 0, also
       erwartet (8,8,8) statt (255,8,8) bei sonst gleichem Block. */
    for (i = 0; i < 4; i++) memcpy(mem + TEX + i * 16u + 8u, &block_zero, 8);
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 8 && mem[o + 2] == 8 && mem[o + 1] == 8 && mem[o + 0] == 0x77,
          "Textur-Cache: geaenderte Quellkachel wird neu dekodiert");
}

static void test_lighting(ArmCpu *c, unsigned char *mem)
{
    static const float view[3] = {0.0f, 0.0f, 1.0f};
    static const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};   /* Identitaet */
    uint32_t *q, size, o;
    uint32_t base;

    /* --- A) Direktionales Licht +z, Diffus rot => Primary-Fragment rot.
       Config0 (0x1C3 Bits4..7 = 0): Distribution0 unterstuetzt, Fresnel nicht.
       config1 0x1C4: Spot (Bit8) und Distanz (Bit24) fuer Licht 0 aus,
       D0-LUT (Bit16) aus. --- */
    base = 0x01010100u;
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);                     /* max_light_index 0 */
    q = emit(q, 0x1C3, 0u);                     /* Config0, kein Shadow/Bump */
    q = emit(q, 0x1C4, base);                   /* Spot/Dist/D0 aus */
    q = emit(q, 0x1D9, 0u);                     /* Slot 0 -> Licht 0 */
    q = emit(q, 0x140, 0u);                     /* specular_0 */
    q = emit(q, 0x141, 0u);                     /* specular_1 */
    q = emit(q, 0x142, 255u << 20);             /* diffuse rot (10 Bit/255) */
    q = emit(q, 0x143, 0u);                     /* ambient */
    q = emit(q, 0x144, f16raw(0.0f) | (f16raw(0.0f) << 16));  /* Position x/y */
    q = emit(q, 0x145, f16raw(1.0f));           /* Position z = +1 (Richtung) */
    q = emit(q, 0x149, 1u);                     /* config: directional */
    q = emit_tev0(q, 1u, 1u, 0u);               /* Quelle PrimaryFragment */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 &&
          mem[o + 0] == 0xFF,
          "Lighting: direktionales Licht +z, Diffus rot => Primary-Fragment rot");

    /* --- B) Secondary-Fragment bleibt 0 (kein Specular). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 2u, 2u, 0u);               /* Quelle SecondaryFragment */
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0,
          "Lighting: Secondary-Fragment ohne Specular => (0,0,0,255)");

    /* --- C) two_sided_diffuse (Bit1) mit Licht -z: abs(dot)=1.
       Ohne Bit1 clampt dot auf 0 => nur Ambient (0) => schwarz. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(-1.0f));          /* nach -z */
    q = emit(q, 0x149, 3u);                     /* directional + two_sided */
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0 && mem[o + 0] == 0xFF,
          "Lighting: two_sided_diffuse nimmt abs(dot) => rot");
    {
        /* gleiche Liste ohne two_sided: dot=-1 -> 0 */
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_quad_light(q, VTX, 8, 8);
        q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
        q = emit(q, 0x1C2, 0u);
        q = emit(q, 0x1C3, 0u);
        q = emit(q, 0x1C4, base);
        q = emit(q, 0x1D9, 0u);
        q = emit(q, 0x142, 255u << 20);
        q = emit(q, 0x145, f16raw(-1.0f));
        q = emit(q, 0x149, 1u);                 /* directional, kein two_sided */
        q = emit_tev0(q, 1u, 1u, 0u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
        check(mem[o + 3] == 0x00 && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 && mem[o + 0] == 0xFF,
              "Lighting: ohne two_sided wird dot<0 auf 0 geclampt => schwarz");
    }

    /* --- D) Distanzdaempfung: Punktlicht im Ursprung, View +z => length 1.
       scale 0.5 + bias 0 => sample_loc 0.5 => LUT-Typ 16 Index 128.
       LUT 4095 (=1.0) => Distanzfaktor 1 (rot), LUT 0 => Faktor 0 (schwarz).
       Damit ist LUT-Upload 0x1C5/0x1C8 und die Index-Inkrementierung belegt. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x00010100u);            /* Spot + D0 aus, Distanz AN */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x144, 0u);                     /* Position (0,0,0) */
    q = emit(q, 0x145, 0u);
    q = emit(q, 0x149, 0u);                     /* Punktlicht */
    q = emit(q, 0x14A, 0u);                     /* dist_atten_bias = 0 */
    q = emit(q, 0x14B, f20raw(0.5f));           /* dist_atten_scale = 0.5 */
    q = emit(q, 0x1C5, (16u << 8) | 128u);      /* Typ DistanceAttenuation, Idx 128 */
    q = emit(q, 0x1C8, 4095u);                  /* Wert 1.0 */
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 && mem[o + 0] == 0xFF,
          "Lighting: Distanz-LUT 0x1C5/0x1C8 Typ 16 Idx 128 = 1.0 => Licht traegt");

    /* gleiche Konfiguration mit LUT-Wert 0 => Abstand daempft vollstaendig */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x00010100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x144, 0u);
    q = emit(q, 0x145, 0u);
    q = emit(q, 0x149, 0u);
    q = emit(q, 0x14A, 0u);
    q = emit(q, 0x14B, f20raw(0.5f));
    q = emit(q, 0x1C5, (16u << 8) | 128u);
    q = emit(q, 0x1C8, 0u);                     /* Wert 0.0 */
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0x00 && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 && mem[o + 0] == 0xFF,
          "Lighting: Distanz-LUT 0 => Diffus faellt auf 0 (nur Alpha bleibt)");

    /* --- E) Fresnel (Config1) setzt Diffus-Alpha (Bits2/3 in 0x1C3).
       LUT-Typ 3 (Fresnel) Index 255 = 2048/4095 = 0.5001 => Alpha 127.
       NH=1 (Normale +z, Licht +z, View +z) => Index 255, Fraktion 1. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, (1u << 4) | (1u << 2) | (1u << 3));  /* Config1 + Alpha */
    q = emit(q, 0x1C4, 0x01010100u);            /* Spot/Dist/D0 aus */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);             /* Diffus rot */
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (3u << 8) | 255u);       /* Typ Fresnel, Index 255 */
    q = emit(q, 0x1C8, 2048u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 && mem[o + 0] == 127,
          "Lighting: Fresnel-LUT Typ 3 setzt Diffus-Alpha 127");

    /* --- F) Bump-Mapping (NormalMap): Stuetztextur (128,128,128) =>
       Perturbation (0.00392,0.00392,0.00392). Mit Renorm ist
       z = sqrt(1 - 2*0.00392^2) = 0.999985 => dot(Licht +z, Normale) 0.999985
       => Diffusion 254. Ohne Renorm (Bit30) bleibt z = 0.00392 => 0. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(mem + TEX, 0, 0x100);
    {
        unsigned char bump[4] = {128, 128, 128, 0};
        t_tex_encode(mem + TEX, 0, (unsigned)t_morton(4, 3), bump);
    }
    t_light_vertices(c, VTX, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x80, 1u);                      /* Textur 0 an (Bump-Quelle) */
    q = emit(q, 0x82, (8u << 16) | 8u);
    q = emit(q, 0x84, 0u);
    q = emit(q, 0x85, TEX >> 3);
    q = emit(q, 0x8E, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, (1u << 28));             /* bump_mode = NormalMap */
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 254 && mem[o + 2] == 0x00 && mem[o + 1] == 0x00 && mem[o + 0] == 0xFF,
          "Lighting: Bump-Map-NormalMap renormiert z => Diffus 254");

    /* gleiche Lage ohne Renorm (Bit30): z bleibt 0.00392 => 0 */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x80, 1u);
    q = emit(q, 0x82, (8u << 16) | 8u);
    q = emit(q, 0x84, 0u);
    q = emit(q, 0x85, TEX >> 3);
    q = emit(q, 0x8E, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, (1u << 30) | (1u << 28));/* NormalMap ohne Renorm */
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] <= 1 && mem[o + 2] == 0 && mem[o + 1] == 0 && mem[o + 0] == 0xFF,
          "Lighting: Bump-Map ohne Renorm => Diffus ~0");
}

/* Review-Tests zum Lighting/PICA-Paket. Erwartungen unabhaengig aus Azahar
 * abgeleitet: sw_lighting.cpp get_lut_value (Abs-Zweig, Spot, D0/D1,
 * Reflect RGB, Clamp, global_ambient), sw_clipper.cpp
 * FlipQuaternionIfOpposite / ConvertCubeCoord, regs_lighting.h BumpMode. */
static void test_lighting_review(ArmCpu *c, unsigned char *mem)
{
    static const float view_z[3] = {0.0f, 0.0f, 1.0f};
    static const float view_nz[3] = {0.0f, 0.0f, -1.0f};
    static const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    /* -(Rotation 60 Grad um x) = (-sin30,0,0,-cos30): dot mit Identitaet < 0,
       beschreibt aber dieselbe Rotation wie (sin30,0,0,cos30). */
    static const float quat_opp[4] = {-0.5f, 0.0f, 0.0f, -0.8660254f};
    uint32_t *q, size, o;
    uint32_t base = 0x01010100u;

    /* --- G) Abs-LUT-Eingang LN = -1: ohne two_sided max(result,0)=0
       (Index 0), mit two_sided |result|=1 (Index 255). LUT 0/255
       unterscheiden sich, damit der Abs-Zweig eindeutig belegt ist. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);                     /* Config0 */
    q = emit(q, 0x1C4, 0x01000100u);            /* Spot/Dist aus, D0 an */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 3u);                     /* D0-Eingang LN */
    q = emit(q, 0x140, 255u << 20);             /* specular_0 rot */
    q = emit(q, 0x145, f16raw(-1.0f));          /* Licht nach -z: LN = -1 */
    q = emit(q, 0x149, 1u);                     /* directional, kein two_sided */
    q = emit(q, 0x1C5, (0u << 8) | 0u);
    q = emit(q, 0x1C8, 0u);                     /* LUT[0][0] = 0 */
    q = emit(q, 0x1C5, (0u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);                  /* LUT[0][255] = 1.0 */
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0,
          "Lighting-Abs: LN=-1 ohne two_sided => max(0,-1)=0 => Index 0 => schwarz");

    /* gleiche Lage mit two_sided_diffuse (lcfg Bit1): fabs(-1)=1 => rot */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x01000100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 3u);
    q = emit(q, 0x140, 255u << 20);
    q = emit(q, 0x145, f16raw(-1.0f));
    q = emit(q, 0x149, 3u);                     /* directional + two_sided */
    q = emit(q, 0x1C5, (0u << 8) | 0u);
    q = emit(q, 0x1C8, 0u);
    q = emit(q, 0x1C5, (0u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0xFF,
          "Lighting-Abs: LN=-1 mit two_sided => fabs => Index 255 => rot");

    /* --- H) Abs-LUT-Eingang NH: Licht (0,sqrt3/2,1/2), View -z =>
       half=(0,sqrt3/2,-1/2), normiert NH=-1/2. Ohne two_sided => 0,
       mit two_sided => 1/2 (Index 127/128 je nach f16-Rundung). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_nz, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x01000100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 0u);                     /* D0-Eingang NH */
    q = emit(q, 0x140, 255u << 20);
    q = emit(q, 0x144, (uint32_t)f16raw(0.8660254f) << 16);
    q = emit(q, 0x145, f16raw(0.5f));
    q = emit(q, 0x149, 1u);                     /* kein two_sided */
    q = emit(q, 0x1C5, (0u << 8) | 127u);
    q = emit(q, 0x1C8, 4095u);
    q = emit(q, 0x1C5, (0u << 8) | 128u);
    q = emit(q, 0x1C8, 4095u);
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0,
          "Lighting-Abs: NH=-1/2 ohne two_sided => max(0,...)=0 => schwarz");

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x01000100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 0u);
    q = emit(q, 0x140, 255u << 20);
    q = emit(q, 0x144, (uint32_t)f16raw(0.8660254f) << 16);
    q = emit(q, 0x145, f16raw(0.5f));
    q = emit(q, 0x149, 3u);                     /* two_sided */
    q = emit(q, 0x1C5, (0u << 8) | 127u);
    q = emit(q, 0x1C8, 4095u);
    q = emit(q, 0x1C5, (0u << 8) | 128u);
    q = emit(q, 0x1C8, 4095u);
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0xFF,
          "Lighting-Abs: NH=-1/2 mit two_sided => fabs => rot");

    /* --- I) Quaternion-Flip: v0 = Identitaet, v1/v2 = -Rotation(60 Grad um
       x). Ohne Flip interpoliert das Dreieck gegen die Gegenorientierung
       (Normale kippt nach -z, dot<0 => schwarz); mit Flip ergibt sich bei
       Pixel (3,0) ~26 Grad um x => dot ~0.897 => rot ~228. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices_q2(c, VTX, 0.0f, 1.0f, view_z, quat, quat_opp);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);             /* Diffus rot */
    q = emit(q, 0x145, f16raw(1.0f));           /* Licht +z */
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 3, 0, 8, 8, 4);
    check(mem[o + 3] >= 220 && mem[o + 3] <= 235 &&
          mem[o + 2] == 0 && mem[o + 1] == 0 && mem[o + 0] == 0xFF,
          "Lighting: FlipQuaternionIfOpposite => Interpolation um x => Diffus ~228");

    /* --- J) bump_mode 3: wie Azahar unbekannt, Normal/Tangente bleiben 0
       (nur LOG_ERROR) => kein Diffus, obwohl Bump-Textur und Licht an sind. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    memset(mem + TEX, 0, 0x100);
    {
        unsigned char bump[4] = {128, 128, 128, 0};
        t_tex_encode(mem + TEX, 0, (unsigned)t_morton(4, 3), bump);
    }
    t_light_vertices(c, VTX, 0.0f, 0.5f, 0.5f, 0.5f, 0.5f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x80, 1u);
    q = emit(q, 0x82, (8u << 16) | 8u);
    q = emit(q, 0x84, 0u);
    q = emit(q, 0x85, TEX >> 3);
    q = emit(q, 0x8E, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 3u << 28);               /* bump_mode 3 (unbekannt) */
    q = emit(q, 0x1C4, base);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0,
          "Lighting: bump_mode 3 => Normal/Tangente 0 => kein Diffus");

    /* --- K) Mehrfach-Licht: zwei direktionale Lichter, Diffus rot + gruen
       => Primary (255,255,0). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 1u);                     /* max_light_index = 1 */
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x03030300u);            /* Spot/Dist/D0/D1 fuer 0+1 aus */
    q = emit(q, 0x1D9, 1u << 4);                /* Slot0 -> Licht0, Slot1 -> Licht1 */
    q = emit(q, 0x142, 255u << 20);             /* Licht0 Diffus rot */
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x152, 255u << 10);             /* Licht1 Diffus gruen */
    q = emit(q, 0x155, f16raw(1.0f));
    q = emit(q, 0x159, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0xFF &&
          mem[o + 3] == 0xFF,
          "Lighting: zwei Lichter rot+gruen => Primary (255,255,0)");

    /* --- L) Spot-Daempfung: Spot-Richtung +z, Licht +z => SP=1 (Index 255).
       Spot-LUT Typ 8 Wert 2048/4095 => Faktor 0.5 => Diffus 127. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x03010000u);            /* Spot an, Dist/D0/D1 aus */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 4u << 8);                /* Spot-Eingang SP */
    q = emit(q, 0x142, 255u << 20);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x146, 0u);                     /* spot x/y = 0 */
    q = emit(q, 0x147, 2047u);                  /* spot z = +1 */
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (8u << 8) | 255u);       /* SpotlightAttenuation Licht 0 */
    q = emit(q, 0x1C8, 2048u);                  /* 2048/4095 = 0.5 */
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 127 && mem[o + 2] == 0 && mem[o + 1] == 0 && mem[o + 0] == 0xFF,
          "Lighting: Spot-LUT 0.5 (SP=1) => Diffus 127");

    /* --- M) D0/D1-Specular: Config2 (Distribution0 und 1). D0=LUT 1.0
       auf specular_0 (rot), D1=LUT 0.5 auf specular_1 (blau), Reflect
       Red = 1.0 => Secondary (255,0,127). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 2u << 4);                /* Config2 */
    q = emit(q, 0x1C4, 0x01000100u);            /* Spot/Dist aus, D0/D1/RR an */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 3u | (1u << 4));         /* D0=LN, D1=VH */
    q = emit(q, 0x140, 255u << 20);             /* specular_0 rot */
    q = emit(q, 0x141, 255u);                   /* specular_1 blau */
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (0u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);                  /* D0 = 1.0 */
    q = emit(q, 0x1C5, (1u << 8) | 255u);
    q = emit(q, 0x1C8, 2048u);                  /* D1 = 0.5 */
    q = emit(q, 0x1C5, (6u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);                  /* ReflectRed = 1.0 */
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 127 &&
          mem[o + 0] == 0xFF,
          "Lighting: D0 rot 1.0 + D1 blau 0.5 + ReflectRed => (255,0,127)");

    /* --- N) Reflect RGB: Config4, D1 = 1.0, ReflectRed/Green/Blue aus
       LUT-Typ 6/5/4 = 1.0 / 0.5 / 0.25 => Secondary (255,127,63). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 4u << 4);                /* Config4 */
    q = emit(q, 0x1C4, base);                   /* D0 aus, D1/RR/RG/RB an */
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, (3u << 16) | (1u << 20));/* RB=LN, RG=VH, RR=NH */
    q = emit(q, 0x141, (255u << 20) | (255u << 10) | 255u);  /* specular_1 weiss */
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (1u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);                  /* D1 = 1.0 */
    q = emit(q, 0x1C5, (6u << 8) | 255u);
    q = emit(q, 0x1C8, 4095u);                  /* ReflectRed = 1.0 */
    q = emit(q, 0x1C5, (5u << 8) | 255u);
    q = emit(q, 0x1C8, 2048u);                  /* ReflectGreen = 0.5 */
    q = emit(q, 0x1C5, (4u << 8) | 255u);
    q = emit(q, 0x1C8, 1024u);                  /* ReflectBlue = 0.25 */
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 3] == 255 && mem[o + 2] == 127 && mem[o + 1] == 63 &&
          mem[o + 0] == 0xFF,
          "Lighting: Reflect RGB 1.0/0.5/0.25 => Secondary (255,127,63)");

    /* --- O) Clamp-Highlights (config0 Bit27): Licht +x, Normale +z =>
       dot=0. D0-LUT (NH=0.7071) liefert 1.0; ohne Clamp rot, mit Clamp 0. --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 1u << 27);               /* clamp_highlights */
    q = emit(q, 0x1C4, 0x01000100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 0u);                     /* D0 = NH */
    q = emit(q, 0x140, 255u << 20);
    q = emit(q, 0x144, f16raw(1.0f));           /* Licht +x */
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (0u << 8) | 181u);
    q = emit(q, 0x1C8, 4095u);                  /* NH=0.7071 => 1.0 */
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0,
          "Lighting: clamp_highlights bei dot=0 => Specular 0");

    /* gleiche Lage ohne Clamp => Specular rot */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x01000100u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x1D1, 0u);
    q = emit(q, 0x140, 255u << 20);
    q = emit(q, 0x144, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit(q, 0x1C5, (0u << 8) | 181u);
    q = emit(q, 0x1C8, 4095u);
    q = emit_tev0(q, 2u, 2u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0 && mem[o + 3] == 0xFF,
          "Lighting: ohne clamp_highlights bleibt das Specular rot");

    /* --- P) Globales Ambient 0x1C0: gruen, Licht ohne Diffus/Ambient =>
       Primary (0,255,0). --- */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_light_vertices(c, VTX, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, view_z, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit(q, 0x1C0, 255u << 10);             /* global_ambient gruen */
    q = emit(q, 0x1C2, 0u);
    q = emit(q, 0x1C3, 0u);
    q = emit(q, 0x1C4, 0x03030300u);
    q = emit(q, 0x1D9, 0u);
    q = emit(q, 0x145, f16raw(1.0f));
    q = emit(q, 0x149, 1u);
    q = emit_tev0(q, 1u, 1u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0 && mem[o + 2] == 0xFF &&
          mem[o + 3] == 0,
          "Lighting: global_ambient gruen => Primary (0,255,0)");
}

static void test_texture_projection_cube(ArmCpu *c, unsigned char *mem)
{
    static const float view[3] = {0.0f, 0.0f, 1.0f};
    static const float quat[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const unsigned char black[4] = {0, 0, 0, 0xFF};
    const unsigned char green[4] = {0, 0xFF, 0, 0xFF};
    unsigned char *tile = mem + TEX;
    uint32_t *q, size, o;
    int i;

    /* 1) Projection2D (Typ 3) + TEXCOORD0_W (Semantik 16): u/v /= tc0_w.
       Textur 4x4 RGBA8, Texel (1,2) gruen (Sampler invertiert t), Rest
       schwarz. Quad u/v konstant 0.140625, tc0_w 0.5 -> u'=0.28125,
       s=t=int(1.125)=1. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(tile, 0, 0x200);
    for (i = 0; i < 16; i++)
        t_tex_encode(tile, 0, (unsigned)t_morton(i % 4, i / 4), black);
    t_tex_encode(tile, 0, (unsigned)t_morton(1, 2), green);
    t_light_vertices(c, VTX, 0.0f, 0.140625f, 0.140625f, 0.140625f, 0.140625f,
                     0.5f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 4, 4, 0, 3u << 28);  /* Typ Projection2D */
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0x00 && mem[o + 2] == 0xFF && mem[o + 3] == 0x00,
          "Projection2D: u/v /= tc0_w (0.5) => Texel (1,2) gruen");

    /* Gegenprobe: Typ Texture2D mit demselben tc0_w ignoriert die Division:
       u=0.140625*4=0.5625 -> s=0, t=0 -> Zeile 3 (schwarz). */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    t_light_vertices(c, VTX, 0.0f, 0.140625f, 0.140625f, 0.140625f, 0.140625f,
                     0.5f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 4, 4, 0, 0u);        /* Typ Texture2D */
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0x00 && mem[o + 2] == 0x00 && mem[o + 3] == 0x00,
          "Texture2D ignoriert tc0_w => Texel (0,3) schwarz");

    /* 2) Cube (Typ 1): (u,v,w)=(0.6,0.2,0.3) -> +X-Face, y=-v, z=u,
       u'=(-0.3/0.6)*0.5+0.5=0.25, v'=(-0.2/0.6)*0.5+0.5=0.3333
       => s=2, t=int(2.666)=2 -> Zeile 5. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(tile, 0, 0x200);
    for (i = 0; i < 64; i++)
        t_tex_encode(tile, 0, (unsigned)i, black);
    t_tex_encode(tile, 0, (unsigned)t_morton(2, 5), green);
    t_light_vertices(c, VTX, 0.0f, 0.6f, 0.2f, 0.6f, 0.2f, 0.3f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 0, 1u << 28);  /* Typ TextureCube */
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 2] == 0xFF && mem[o + 1] == 0x00 && mem[o + 3] == 0x00 && mem[o + 0] == 0xFF,
          "TextureCube: +X-Face, y=-v, z=u => Texel (2,5) gruen");

    /* Gegenprobe: Texture2D mit (0.6,0.2) => s=4, t=1 -> Zeile 6 (schwarz). */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    t_light_vertices(c, VTX, 0.0f, 0.6f, 0.2f, 0.6f, 0.2f, 0.3f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 0, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0xFF && mem[o + 1] == 0x00 && mem[o + 2] == 0x00 && mem[o + 3] == 0x00,
          "Texture2D mit denselben Koordinaten => Zeile 6 schwarz");

    /* 3) Shadow2D (Typ 2): u/v /= tc0_w (nicht orthographisch),
       shadow_z = |tc0_w|. Texel (4,3) mit Rot 0x5A und z_ref 0.
       tc0_w=0.5 -> z_int=0x7FFFFF > 0 => Density 0. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(tile, 0, 0x200);
    for (i = 0; i < 64; i++)
        t_tex_encode(tile, 0, (unsigned)i, black);
    {
        unsigned char ref[4] = {0x5A, 0, 0, 0};
        t_tex_encode(tile, 0, (unsigned)t_morton(4, 3), ref);
    }
    t_light_vertices(c, VTX, 0.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.5f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 0, 2u << 28);  /* Typ Shadow2D */
    q = emit(q, 0x8B, 0u);                      /* orthographic=0, bias=0 */
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0 && mem[o + 3] == 0,
          "Shadow2D: z_ref 0 < z_int 0x7FFFFF => Density 0");

    /* Bias 0x400001 (Bits1..23) hebt z_int auf -1: z_ref 0 >= -1 => rot. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    t_light_vertices(c, VTX, 0.0f, 0.25f, 0.25f, 0.25f, 0.25f, 0.5f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 0, 2u << 28);
    q = emit(q, 0x8B, 0x400001u << 1);          /* bias 0x400001 */
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0x5A && mem[o + 1] == 0x5A && mem[o + 3] == 0x5A,
          "Shadow2D: bias verschiebt z_int unter z_ref => Density 0x5A");

    /* 4) ShadowCube (Typ 4): +X-Face wie oben, shadow_z=|z|=0.6,
       Texel (2,5) mit Rot 0x44 und z_ref 0xFFFFFF => Density 0x44. */
    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x200);
    memset(tile, 0, 0x200);
    for (i = 0; i < 64; i++)
        t_tex_encode(tile, 0, (unsigned)i, black);
    {
        unsigned char ref[4] = {0x44, 0xFF, 0xFF, 0xFF};
        t_tex_encode(tile, 0, (unsigned)t_morton(2, 5), ref);
    }
    t_light_vertices(c, VTX, 0.0f, 0.6f, 0.2f, 0.6f, 0.2f, 0.3f, view, quat);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_quad_light(q, VTX, 8, 8);
    q = t_emit_tex(q, TEX, 8, 8, 0, 4u << 28);  /* Typ ShadowCube */
    q = emit(q, 0x8B, 0u);
    q = t_emit_fb(q, 0x40000u, 8, 8, 0u);
    q = emit_tev0(q, 3u, 3u, 0u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 4, 4, 8, 8, 4);
    check(mem[o + 0] == 0x44 && mem[o + 1] == 0x44 && mem[o + 2] == 0x44 &&
          mem[o + 3] == 0x44,
          "ShadowCube: Face +X, shadow_z 0.6, z_ref 0xFFFFFF => Density 0x44");
}

/* ================== Geometrie-Shader (Shader-Einheit 3) ==================
 * Azahar geometry_pipeline.cpp / shader_unit.cpp (GeometryEmitter) /
 * regs_pipeline.h / pica_core.cpp: Register 0x280..0x2AF (Programm 0x29B,
 * Swizzle 0x2A5, 0x289 input_buffer_config, 0x28A main_offset, 0x28B/0x28C
 * Input-Map, 0x28D output_mask), Pipeline 0x229/0x244/0x24A/0x252/0x254 und
 * EMIT 0x2A / SETEMIT 0x2B (Bits0..1 vertex_id, Bit2 prim_emit, Bit3 winding).
 * Erwartungswerte kommen aus der Azahar-Semantik, nicht aus dem Port:
 * die drei VS-Ausgaben fuellen den GS-Eingangspuffer, der GS emittiert sie in
 * der Reihenfolge der EMITs, die Primitive Assembly (Topologie Shader) bildet
 * daraus das Dreieck. */

/* SETEMIT-Kodierung (nihstro shader_bytecode.h). */
static uint32_t ins_setemit(uint32_t vertex_id, uint32_t prim_emit, uint32_t winding)
{
    return (0x2Bu << 26) | (vertex_id & 3u) | ((prim_emit & 1u) << 2) |
           ((winding & 1u) << 3);
}

static uint32_t *emit_gs_program(uint32_t *p, const uint32_t *words, int n)
{
    int i;
    p = emit(p, 0x29B, 0);
    for (i = 0; i < n; i++) p = emit(p, 0x29C, words[i]);
    return p;
}

static uint32_t *emit_gs_swizzle0(uint32_t *p, uint32_t w)
{
    p = emit(p, 0x2A5, 0);
    return emit(p, 0x2A6, w);
}

/* GS-Programm: mov oX, vi + EMIT fuer i = 0..2; das letzte mit prim_emit. */
static int t_gs_words_emit3(uint32_t *w, uint32_t dest, int winding)
{
    int i, n = 0;
    for (i = 0; i < 3; i++) {
        w[n++] = ins_common(0x13u, 0u, (uint32_t)i, 0u, dest, 0u);
        w[n++] = ins_setemit((uint32_t)i, (i == 2) ? 1u : 0u,
                             (i == 2) ? (uint32_t)winding : 0u);
        w[n++] = 0x2Au << 26;
    }
    w[n++] = 0x22u << 26;
    return n;
}

/* Rahmen: Viewport 8x8, TEV-Konstante rot, RGBA8-Farbziel 8x8. */
static uint32_t *t_emit_fb_red8(uint32_t *q)
{
    q = emit_viewport(q, 8, 8);
    q = emit_tev0(q, 0xEu, 0xEu, 0xFF0000FFu);
    return t_emit_fb(q, 0x40000u, 8, 8, 0u);
}

static void test_gs_point(ArmCpu *c, unsigned char *mem)
{
    uint32_t w[10], *q, size, o;
    int n;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
    t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);            /* VS: attr0->v0, o0->POSITION, mask 1 */
    q = emit(q, 0x229, 2u);             /* use_gs = Yes */
    q = emit(q, 0x24A, 0u);             /* 1 VS-Ausgangsattribut */
    q = emit(q, 0x251, 0u);
    q = emit(q, 0x252, 0u);             /* GSMode Point */
    q = emit(q, 0x25E, 3u << 8);        /* Topologie Shader */
    q = emit(q, 0x289, 2u);             /* 3 GS-Eingangsattribute */
    q = emit(q, 0x28B, 0x210u);         /* attr0->v0, attr1->v1, attr2->v2 */
    q = emit(q, 0x28C, 0u);
    q = emit(q, 0x28A, 0u);             /* GS-Einstieg bei Wort 0 */
    q = emit(q, 0x28D, 1u);             /* GS output_mask = o0 */
    n = t_gs_words_emit3(w, 0u, 0);
    q = emit_gs_program(q, w, n);
    q = emit_gs_swizzle0(q, swz_xyzw());
    q = t_emit_fb_red8(q);
    q = emit(q, 0x228, 3u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
          "GS Point: 3 VS-Ausgaben fuellen den Puffer, 3 EMITs => Dreieck");
}

static void test_gs_fixed_primitive(ArmCpu *c, unsigned char *mem)
{
    uint32_t w[10], *q, size, o;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
    t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit(q, 0x229, 2u);
    q = emit(q, 0x24A, 0u);
    q = emit(q, 0x251, 0u);
    q = emit(q, 0x252, 2u | (2u << 8)); /* FixedPrimitive, 3 Vertices, Stride 1 */
    q = emit(q, 0x25E, 3u << 8);
    q = emit(q, 0x289, 1u << 8);        /* input_to_uniform = 1 */
    q = emit(q, 0x28A, 0u);
    q = emit(q, 0x28D, 1u);
    /* FixedPrimitive: die Vertexdaten stehen in den Uniforms f[0..2],
       das Programm liest sie als c0..c2. */
    w[0] = ins_common(0x13u, 0u, 0x20u, 0u, 0u, 0u);
    w[1] = ins_setemit(0u, 0u, 0u);
    w[2] = 0x2Au << 26;
    w[3] = ins_common(0x13u, 0u, 0x21u, 0u, 0u, 0u);
    w[4] = ins_setemit(1u, 0u, 0u);
    w[5] = 0x2Au << 26;
    w[6] = ins_common(0x13u, 0u, 0x22u, 0u, 0u, 0u);
    w[7] = ins_setemit(2u, 1u, 0u);
    w[8] = 0x2Au << 26;
    w[9] = 0x22u << 26;
    q = emit_gs_program(q, w, 10);
    q = emit_gs_swizzle0(q, swz_xyzw());
    q = t_emit_fb_red8(q);
    q = emit(q, 0x228, 3u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
          "GS FixedPrimitive: Vertexdaten in Uniforms f0..f2, 3 EMITs => Dreieck");
}

static void test_gs_variable_primitive(ArmCpu *c, unsigned char *mem)
{
    uint32_t w[10], *q, size, o;
    uint16_t idx[4];

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
    t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
    idx[0] = 3; idx[1] = 0; idx[2] = 1; idx[3] = 2;
    memcpy(mem + ((VTX + 0x300u) & 0x1FFFFFFFu), idx, sizeof(idx));
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit(q, 0x229, 2u | (1u << 31));   /* use_gs Yes + variable_primitive */
    q = emit(q, 0x24A, 0u);
    q = emit(q, 0x251, 0u);
    q = emit(q, 0x252, 1u);                /* GSMode VariablePrimitive */
    q = emit(q, 0x254, 3u);                /* 4 Hauptvertices (alle) */
    q = emit(q, 0x25E, 3u << 8);
    q = emit(q, 0x289, 1u << 8);           /* input_to_uniform = 1 */
    q = emit(q, 0x227, 0x300u | 0x80000000u);  /* u16-Indexpuffer ab Basis */
    q = emit(q, 0x28A, 0u);
    q = emit(q, 0x28D, 1u);
    /* f[0] = Vertexzahl, f[1..3] = Vertexdaten (c1..c3). */
    w[0] = ins_common(0x13u, 0u, 0x21u, 0u, 0u, 0u);
    w[1] = ins_setemit(0u, 0u, 0u);
    w[2] = 0x2Au << 26;
    w[3] = ins_common(0x13u, 0u, 0x22u, 0u, 0u, 0u);
    w[4] = ins_setemit(1u, 0u, 0u);
    w[5] = 0x2Au << 26;
    w[6] = ins_common(0x13u, 0u, 0x23u, 0u, 0u, 0u);
    w[7] = ins_setemit(2u, 1u, 0u);
    w[8] = 0x2Au << 26;
    w[9] = 0x22u << 26;
    q = emit_gs_program(q, w, 10);
    q = emit_gs_swizzle0(q, swz_xyzw());
    q = t_emit_fb_red8(q);
    q = emit(q, 0x228, 4u);                /* Index 0 = Vertexzahl, dann 3 */
    q = emit(q, 0x22F, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
          "GS VariablePrimitive: erster Index = Vertexzahl, Daten in f1..f3 => Dreieck");
}

/* 0x244 gs_unit_exclusive_configuration + Programm-Spiegelung: bei 0/0 teilt
   Einheit 3 das VS-Programm (Azahar pica_core.cpp), bei 1 bleibt ihr Programm
   leer und ohne EMIT entsteht kein Dreieck. */
static void test_gs_exclusive_program_share(ArmCpu *c, unsigned char *mem)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        uint32_t w[11], *q, size, o;

        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
        t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
        t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x200, VTX >> 3);
        q = emit(q, 0x201, 0x0Fu);
        q = emit(q, 0x202, 0u);
        q = emit(q, 0x203, 0u);
        q = emit(q, 0x204, 0u);
        q = emit(q, 0x205, (1u << 28) | (16u << 16));
        q = emit(q, 0x244, pass ? 1u : 0u);   /* exclusive configuration */
        q = emit(q, 0x229, 0u);               /* beim Upload: GS aus */
        q = emit(q, 0x2B9, 0u);
        q = emit(q, 0x2BB, 0u);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);               /* VS-Ausgang nur o0 (Position) */
        /* Gemeinsames Programm: VS schreibt o0, der GS emittiert o1. */
        w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);   /* mov o0,v0 */
        w[1] = ins_common(0x13u, 0u, 0x00u, 0u, 1u, 0u);   /* mov o1,v0 */
        w[2] = ins_setemit(0u, 0u, 0u);
        w[3] = 0x2Au << 26;
        w[4] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);   /* mov o1,v1 */
        w[5] = ins_setemit(1u, 0u, 0u);
        w[6] = 0x2Au << 26;
        w[7] = ins_common(0x13u, 0u, 0x02u, 0u, 1u, 0u);   /* mov o1,v2 */
        w[8] = ins_setemit(2u, 1u, 0u);
        w[9] = 0x2Au << 26;
        w[10] = 0x22u << 26;
        q = emit_program(q, w, 11);
        q = emit_swizzle0(q, swz_xyzw());
        /* Draw mit aktivem GS (Point, 3 Eingangsattribute, Ausgang o1). */
        q = emit(q, 0x229, 2u);
        q = emit(q, 0x24A, 0u);
        q = emit(q, 0x251, 0u);
        q = emit(q, 0x252, 0u);
        q = emit(q, 0x25E, 3u << 8);
        q = emit(q, 0x289, 2u);
        q = emit(q, 0x28B, 0x210u);
        q = emit(q, 0x28C, 0u);
        q = emit(q, 0x28A, 0u);
        q = emit(q, 0x28D, 2u);               /* GS output_mask = o1 */
        q = t_emit_fb_red8(q);
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
        if (pass) {
            check(mem[o + 3] == 0 && mem[o + 2] == 0,
                  "GS exclusive (0x244=1): Programm nicht gespiegelt => kein EMIT, kein Dreieck");
        } else {
            check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
                  "GS Programm-Spiegelung (0x244=0, 0x229=0 beim Upload) => EMIT laeuft");
        }
    }
}

/* SETEMIT-winding invertiert in Topologie Shader die Windung des naechsten
   Dreiecks (Azahar primitive_assembly.cpp). Mit Cull-Modus KeepClockWise
   (0x40=1) wird das Dreieck (-1,-1),(1,-1),(-1,1) nur mit winding=1 gezeichnet. */
static void test_gs_winding(ArmCpu *c, unsigned char *mem)
{
    int pass;
    for (pass = 0; pass < 2; pass++) {
        uint32_t w[10], *q, size, o;
        int n;

        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
        t_pos4(c, 16, 1.0f, -1.0f, 0.0f);
        t_pos4(c, 32, -1.0f, 1.0f, 0.0f);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_pos_setup(q);
        q = emit(q, 0x40, 1u);              /* Cull: nur im Uhrzeigersinn */
        q = emit(q, 0x229, 2u);
        q = emit(q, 0x24A, 0u);
        q = emit(q, 0x251, 0u);
        q = emit(q, 0x252, 0u);
        q = emit(q, 0x25E, 3u << 8);        /* Topologie Shader */
        q = emit(q, 0x289, 2u);
        q = emit(q, 0x28B, 0x210u);
        q = emit(q, 0x28C, 0u);
        q = emit(q, 0x28A, 0u);
        q = emit(q, 0x28D, 1u);
        n = t_gs_words_emit3(w, 0u, pass);
        q = emit_gs_program(q, w, n);
        q = emit_gs_swizzle0(q, swz_xyzw());
        q = t_emit_fb_red8(q);
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22E, 0u);
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
        if (pass) {
            check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
                  "GS SETEMIT winding=1 invertiert die Windung => Dreieck trotz Culling");
        } else {
            check(mem[o + 3] == 0 && mem[o + 2] == 0,
                  "GS ohne winding: Windung bleibt, KeepClockWise verwirft das Dreieck");
        }
    }
}

/* Doppelte Ausgangsbank (Azahar shader_unit.h ShaderUnit::output/output_bank):
   EMIT liest die aktuell beschriebene Bank und wechselt danach. Ein EMIT ohne
   Schreibzugriff liefert daher die Werte von vor zwei EMITs; hier wird damit
   das zuvor emittierte Vertex 1 erneut gesetzt und das Dreieck vollstaendig.
   Ohne Bankwechsel waere das dritte gelesene Vertex C statt B und das Dreieck
   degeneriert (A,C,C) => kein Pixel. */
static void test_gs_output_bank(ArmCpu *c, unsigned char *mem)
{
    uint32_t w[12], *q, size, o;
    int n = 0;

    gpu_pica_reset();
    memset(mem + 0x40000u, 0, 0x100);
    t_pos4(c, 0, -1.0f, -1.0f, 0.0f);
    t_pos4(c, 16, -1.0f, 1.0f, 0.0f);
    t_pos4(c, 32, 1.0f, -1.0f, 0.0f);
    q = (uint32_t *)(mem + LIST);
    q = t_emit_pos_setup(q);
    q = emit(q, 0x229, 2u);
    q = emit(q, 0x24A, 0u);
    q = emit(q, 0x251, 0u);
    q = emit(q, 0x252, 0u);
    q = emit(q, 0x25E, 3u << 8);
    q = emit(q, 0x289, 2u);
    q = emit(q, 0x28B, 0x210u);
    q = emit(q, 0x28C, 0u);
    q = emit(q, 0x28A, 0u);
    q = emit(q, 0x28D, 1u);
    w[n++] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);   /* mov o0,v0 */
    w[n++] = ins_setemit(0u, 0u, 0u);
    w[n++] = 0x2Au << 26;
    w[n++] = ins_common(0x13u, 0u, 0x01u, 0u, 0u, 0u);   /* mov o0,v1 */
    w[n++] = ins_setemit(1u, 0u, 0u);
    w[n++] = 0x2Au << 26;
    w[n++] = ins_common(0x13u, 0u, 0x02u, 0u, 0u, 0u);   /* mov o0,v2 */
    w[n++] = ins_setemit(2u, 0u, 0u);
    w[n++] = 0x2Au << 26;
    w[n++] = ins_setemit(1u, 1u, 0u);                    /* prim_emit ohne Schreiben */
    w[n++] = 0x2Au << 26;
    w[n++] = 0x22u << 26;
    q = emit_gs_program(q, w, n);
    q = emit_gs_swizzle0(q, swz_xyzw());
    q = t_emit_fb_red8(q);
    q = emit(q, 0x228, 3u);
    q = emit(q, 0x22E, 0u);
    size = (uint32_t)((unsigned char *)q - (mem + LIST));
    gpu_run_pica_list(c, LIST, size);
    o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
    check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
          "GS Ausgangsbank: EMIT ohne Schreiben liest die Werte von vor zwei EMITs");
}

/* Immediate-Modus (0x232 = 0xF, Attribute ueber 0x233..0x235) geht wie in
 * Azahar PicaCore::DrawImmediate durch die Geometrie-Pipeline: erst der
 * dritte Immediate-Vertex fuellt den Point-Puffer und startet den GS. Der
 * SETEMIT-winding des GS wirkt daher auch hier; ohne Pipeline-Unterstuetzung
 * wuerde die Immediate-Ausgabe direkt in die Primitive Assembly gehen und
 * der Cull-Modus KeepClockWise das Dreieck verwerfen.
 * Azahar immediate.Reset() (Schreiben von 0x232) setzt
 * reset_geometry_pipeline; der naechste Flush konfiguriert die Pipeline neu. */
static void test_gs_immediate(ArmCpu *c, unsigned char *mem)
{
    static const float pos[3][3] = {
        { -1.0f, -1.0f, 0.0f }, { 1.0f, -1.0f, 0.0f }, { -1.0f, 1.0f, 0.0f }
    };
    int pass;

    for (pass = 0; pass < 2; pass++) {
        uint32_t w[10], *q, size, o, pack[3];
        int n, v, i;

        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x100);
        q = (uint32_t *)(mem + LIST);
        q = t_emit_pos_setup(q);
        q = emit(q, 0x40, 1u);              /* Cull: nur im Uhrzeigersinn */
        q = emit(q, 0x242, 0u);             /* 1 Immediate-Attribut je Vertex */
        q = emit(q, 0x229, 2u);
        q = emit(q, 0x24A, 0u);
        q = emit(q, 0x251, 0u);
        q = emit(q, 0x252, 0u);             /* Point */
        q = emit(q, 0x25E, 3u << 8);
        q = emit(q, 0x289, 2u);             /* 3 GS-Eingangsattribute */
        q = emit(q, 0x28B, 0x210u);
        q = emit(q, 0x28C, 0u);
        q = emit(q, 0x28A, 0u);
        q = emit(q, 0x28D, 1u);
        n = t_gs_words_emit3(w, 0u, (uint32_t)pass);
        q = emit_gs_program(q, w, n);
        q = emit_gs_swizzle0(q, swz_xyzw());
        q = t_emit_fb_red8(q);
        /* Immediate-Modus: je drei f24-Woerter ein Attribut (ein Vertex). */
        q = emit(q, 0x232, 0xFu);
        for (v = 0; v < 3; v++) {
            f24_pack3(pos[v][0], pos[v][1], pos[v][2], 1.0f, pack);
            for (i = 0; i < 3; i++) q = emit(q, 0x233u + (uint32_t)i, pack[i]);
        }
        size = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(c, LIST, size);
        o = t_fb_off(0x40000u, 1, 5, 8, 8, 4);
        if (pass) {
            check(mem[o + 3] == 0xFF && mem[o + 2] == 0 && mem[o + 1] == 0,
                  "GS Immediate: 3 Immediate-Vertices fuellen den Puffer, winding=1 dreht");
        } else {
            check(mem[o + 3] == 0 && mem[o + 2] == 0,
                  "GS Immediate ohne winding: KeepClockWise verwirft das Dreieck");
        }
    }
}

/* =============== Performance-Benchmark (Diagnose, PICA_BENCH=1) ===============
 * 2000 texturierte Dreiecke (40x25 Quads) ueber den ganzen oberen Screen
 * (400x240) in einer PICA-Kommandoliste, ein Draw ueber 6000 Vertices.
 * Drei Varianten mit identischem Gitter:
 *   tex        Textur + TEV, Lighting aus, kein Depth-Test (2D-UI-Pfad)
 *   tex+light  zusaetzlich Fragment-Lighting mit einer diffusen Lichtquelle
 *   tex+depth  zusaetzlich D16-Depth-Test (LessThan) mit Depth-Write
 * Gemessen wird die Wanduhrzeit pro vollstaendiger Liste (25% Warmlauf).
 * Der Benchmark veraendert keinen Gastzustand und laeuft nur mit PICA_BENCH=1;
 * ohne die Variable ist die Ausgabe von tools/test_pica_c.py unveraendert. */

#define BENCH_MEM_SIZE 0x400000u
#define BENCH_FB       0x00040000u
#define BENCH_TEX      0x000A0000u
#define BENCH_VTX      0x000E0000u
#define BENCH_DEPTH    0x00100000u
#define BENCH_LIST     0x000FE000u
#define BENCH_QUADS_X  40
#define BENCH_QUADS_Y  25
#define BENCH_TRIS     (BENCH_QUADS_X * BENCH_QUADS_Y * 2)
#define BENCH_VERTS    (BENCH_TRIS * 3)

static double bench_now_ms(void)
{
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
#endif
}

/* Gitter erzeugen: 6 Vertices je Quad (Position float3, UV float2, Stride 20). */
static void bench_vertices(unsigned char *mem)
{
    int cx, cy, k;
    for (cy = 0; cy < BENCH_QUADS_Y; cy++) {
        for (cx = 0; cx < BENCH_QUADS_X; cx++) {
            uint32_t q = (uint32_t)(cy * BENCH_QUADS_X + cx);
            float x0 = -1.0f + 2.0f * (float)cx / (float)BENCH_QUADS_X;
            float x1 = -1.0f + 2.0f * (float)(cx + 1) / (float)BENCH_QUADS_X;
            float y0 = -1.0f + 2.0f * (float)cy / (float)BENCH_QUADS_Y;
            float y1 = -1.0f + 2.0f * (float)(cy + 1) / (float)BENCH_QUADS_Y;
            float u0 = (float)cx / (float)BENCH_QUADS_X;
            float u1 = (float)(cx + 1) / (float)BENCH_QUADS_X;
            float v0 = (float)cy / (float)BENCH_QUADS_Y;
            float v1 = (float)(cy + 1) / (float)BENCH_QUADS_Y;
            float xy[6][2] = {{x0, y0}, {x1, y0}, {x1, y1},
                              {x0, y0}, {x1, y1}, {x0, y1}};
            for (k = 0; k < 6; k++) {
                float f[5];
                uint32_t off = BENCH_VTX + (q * 6u + (uint32_t)k) * 20u;
                f[0] = xy[k][0];
                f[1] = xy[k][1];
                f[2] = 0.0f;
                f[3] = (xy[k][0] == x0) ? u0 : u1;
                f[4] = (xy[k][1] == y0) ? v0 : v1;
                memcpy(mem + off, f, sizeof(f));
            }
        }
    }
}

static uint32_t bench_build_list(unsigned char *mem, int lighting, int depth,
                                int texture, int fmt)
{
    uint32_t *q = (uint32_t *)(mem + BENCH_LIST);
    uint32_t *end;

    q = emit(q, 0x200, BENCH_VTX >> 3);
    q = emit(q, 0x201, 0x7B);              /* attr0 float3, attr1 float2 */
    q = emit(q, 0x202, 1u << 28);
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);
    q = emit(q, 0x205, (3u << 28) | (20u << 16));
    q = emit(q, 0x206, 12u);
    q = emit(q, 0x207, 1u);
    q = emit(q, 0x208, (1u << 28) | (20u << 16));
    q = emit(q, 0x228, (uint32_t)BENCH_VERTS);
    q = t_emit_vs_basic(q);
    q = emit_viewport(q, 400, 240);
    if (texture) {
        q = t_emit_tex(q, BENCH_TEX, 256, 256, fmt, 0x00001100u);  /* Repeat */
        q = emit_tev0(q, 3, 3, 0);         /* Farbe+Alpha aus Textur 0 */
    } else {
        q = emit(q, 0x80, 0u);             /* Textur 0 aus */
        q = emit_tev0(q, 0, 0, 0);         /* Farbe+Alpha = Vertexfarbe */
    }
    q = t_emit_fb(q, BENCH_FB, 400, 240, 0u);
    if (lighting) {
        q = emit(q, 0x1C6, 0u);            /* lighting.disable = 0 */
        q = emit(q, 0x142, 0x000003FFu);   /* Licht 0 diffusiv rot */
        q = emit(q, 0x1C2, 0u);            /* max_light_index */
        q = emit(q, 0x1D9, 1u);            /* Licht 0 an */
    } else {
        q = emit(q, 0x1C6, 1u);            /* Lighting aus */
    }
    if (depth) {
        q = emit(q, 0x116, 0u);            /* D16 */
        q = emit(q, 0x11C, BENCH_DEPTH >> 3);
        q = emit(q, 0x107, 0x00001F40u);   /* test LessThan, Maske, write */
    }
    q = emit(q, 0x22E, 0u);                /* Draw arrays */
    end = q;
    return (uint32_t)((unsigned char *)end - (mem + BENCH_LIST));
}

static void bench_one(unsigned char *mem, int lighting, int depth, int texture,
                      int fmt, const char *name)
{
    ArmCpu c;
    uint32_t size = bench_build_list(mem, lighting, depth, texture, fmt);
    double ms, best = 0.0;
    int i, frames = 8, warm = 2;
    uint32_t px = 400u * 240u;

    memset(&c, 0, sizeof(c));
    arm_cpu_init(&c, mem, BENCH_MEM_SIZE);
    gpu_pica_reset();
    memset(mem + BENCH_FB, 0, px * 4u);
    if (depth) {
        memset(mem + BENCH_DEPTH, 0xFF, px * 2u);
    }
    for (i = 0; i < warm + frames; i++) {
        double a, b;
        if (depth && i == warm) memset(mem + BENCH_DEPTH, 0xFF, px * 2u);
        a = bench_now_ms();
        gpu_run_pica_list(&c, BENCH_LIST, size);
        b = bench_now_ms();
        if (i == warm) best = b - a;
        else if (i > warm && b - a < best) best = b - a;
    }
    ms = best;
    printf("BENCH %-10s tris=%d px=%u  %.3f ms/frame  (%5.1f fps)  draws=%llu\n",
           name, BENCH_TRIS, px, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
           gpu_pica_draw_count());
}

static int pica_bench(void)
{
    unsigned char *mem = (unsigned char *)malloc(BENCH_MEM_SIZE);
    int i;
    uint32_t *t;

    if (!mem) {
        printf("BENCH: kein Speicher\n");
        return 1;
    }
    memset(mem, 0, BENCH_MEM_SIZE);
    bench_vertices(mem);
    /* 256x256-RGBA8-Textur mit einem Muster (alle Kacheln belegt). */
    t = (uint32_t *)(mem + BENCH_TEX);
    for (i = 0; i < 256 * 256; i++)
        t[i] = 0xFF000000u | ((uint32_t)(i * 2654435761u) >> 8);
    printf("BENCH PICA-Rasterizer: %d Dreiecke, 400x240\n", BENCH_TRIS);
    bench_one(mem, 0, 0, 1, 0, "tex");
    bench_one(mem, 0, 0, 0, 0, "solid");
    bench_one(mem, 1, 0, 1, 0, "tex+light");
    bench_one(mem, 0, 0, 1, 13, "etc1a4");
    bench_one(mem, 0, 1, 1, 0, "tex+depth");
    free(mem);
    return 0;
}

int main(void) {

    static unsigned char mem[0x100000];
    ArmCpu c;
    uint32_t *p;
    uint32_t size;
    uint32_t px, cx, cy;
    int i;

    memset(&c, 0, sizeof(c));
    memset(mem, 0, sizeof(mem));
    arm_cpu_init(&c, mem, sizeof(mem));

    /* Textur: 2x2 RGB8, alle Texel rot. */
    for (i = 0; i < 4; i++) {
        /* RGB8 liegt wie bei Azahar (common/color.h) als b,g,r im Speicher. */
        mem[TEX + i * 3 + 0] = 0;
        mem[TEX + i * 3 + 1] = 0;
        mem[TEX + i * 3 + 2] = 255;
    }
    /* 3 Vertices, Stride 20: Position float3, UV float2. */
    set_vertex(&c, 0, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
    set_vertex(&c, 20, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
    set_vertex(&c, 40, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);

    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x200, VTX >> 3);          /* (reg>>1)*16 = VTX */
    p = emit(p, 0x201, 0x7B);              /* attr0 float3, attr1 float2 */
    p = emit(p, 0x202, 1u << 28);          /* max_attribute_index = 1 */
    p = emit(p, 0x203, 0);                 /* Loader 0: data_offset 0 */
    p = emit(p, 0x204, 0x210);             /* comp0..2 = attr 0,1,2 */
    p = emit(p, 0x205, (3u << 28) | (20u << 16));  /* cc=3, stride=20 */
    p = emit(p, 0x206, 12);                /* Loader 1: UV bei +12 */
    p = emit(p, 0x207, 1);                 /* comp0 = attr 1 */
    p = emit(p, 0x208, (1u << 28) | (20u << 16));
    p = emit(p, 0x228, 3);                 /* num_vertices */
    p = emit(p, 0x80, 1);                  /* Textur an (Bit 0) */
    p = emit(p, 0x82, (2u << 16) | 2u);    /* 2x2 */
    p = emit(p, 0x85, TEX >> 3);
    p = emit(p, 0x8E, 1);                  /* RGB8 */
    p = t_emit_vs_basic(p);
    p = emit_viewport(p, 400, 240);        /* 0x41/0x43/0x68 */
    p = emit_tev0(p, 3, 0, 0);             /* TEV: Farbe Texture0, Alpha Vertex */
    p = t_emit_fb(p, FB, 400, 240, 0u);    /* RGBA8-Farbziel 0x11D..0x11E */
    p = emit(p, 0x22E, 0);                 /* Draw-Trigger (arrays) */
    size = (uint32_t)((unsigned char *)p - (mem + LIST));

    memset(mem + FB, 0, 400u * 240u * 4u);
    gpu_set_fb(FB);
    gpu_set_fb_size(FB, 400, 240);
    gpu_run_pica_list(&c, LIST, size);

    check(gpu_pica_texture_bound() == 1, "0x80 Bit0 schaltet Textur 0 ein");
    check(gpu_pica_list_count() == 1, "eine abgeschlossene Liste gezaehlt");
    check(gpu_pica_draw_count() == 1, "ein Draw gezaehlt");
    check(gpu_get_fb_width() == 400 && gpu_get_fb_height() == 240,
          "Framebuffer 400x240");

    cx = t_fb_off(FB, 200, 120, 400, 240, 4);
    check(mem[cx + 3] == 255 && mem[cx + 2] == 0 && mem[cx + 1] == 0 && mem[cx + 0] == 255,
          "Mitte rot (Textur ueber 0x200-Loader/Stride 16 gesampelt)");
    px = t_fb_off(FB, 399, 239, 400, 240, 4);
    check(mem[px + 3] == 0 && mem[px + 2] == 0 && mem[px + 1] == 0 && mem[px + 0] == 0,
          "Ecke (399,239) ausserhalb des Dreiecks bleibt schwarz");

    /* Textur aus: Azahar liefert fuer eine deaktivierte Textureinheit
       0,0,0,0 (kein Weiss-Fallback); TEV-Quelle Texture0 wird schwarz. */
    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x80, 0);
    p = emit(p, 0x22E, 0);
    size = (uint32_t)((unsigned char *)p - (mem + LIST));
    gpu_run_pica_list(&c, LIST, size);
    check(gpu_pica_texture_bound() == 0, "0x80=0 schaltet Textur 0 aus");
    check(mem[cx + 3] == 0 && mem[cx + 2] == 0 && mem[cx + 1] == 0,
          "Mitte schwarz ohne Textur (TEV-Quelle Texture0 = 0)");

    /* ---- Adressumrechnung physisch -> Gast-Linearspeicher ----
     * Azahar memory.h: VRAM_PADDR 0x18000000 / VRAM_VADDR 0x1F000000,
     * FCRAM_PADDR 0x20000000; der Port legt den Linear-Heap ab 0x14000000. */
    check(gpu_pica_phys_to_guest(0x18000000u) == 0x1F000000u,
          "VRAM 0x18000000 -> Gast 0x1F000000");
    check(gpu_pica_phys_to_guest(0x18001234u) == 0x1F001234u,
          "VRAM-Offset bleibt erhalten");
    check(gpu_pica_phys_to_guest(0x20000000u) == 0x14000000u,
          "FCRAM 0x20000000 -> Linear-Heap 0x14000000");
    check(gpu_pica_phys_to_guest(0x20001234u) == 0x14001234u,
          "FCRAM-Offset bleibt erhalten");
    check(gpu_pica_phys_to_guest(0x1F000000u) == 0x1F000000u,
          "virtuelle VRAM-Adresse bleibt unveraendert");
    check(gpu_pica_virt_to_phys(0x1F001234u) == 0x18001234u,
          "GSP: 0x1F001234 -> physisch 0x18001234");
    check(gpu_pica_virt_to_phys(0x14001234u) == 0x20001234u,
          "GSP: 0x14001234 -> physisch 0x20001234");
    check(gpu_pica_virt_to_phys(0x00005000u) == 0x00005000u,
          "unbekannte Adresse bleibt unveraendert");

    /* ---- Header-Maske + Consecutive-Writes + 8-Byte-Ausrichtung ---- */
    gpu_pica_reset();
    p = (uint32_t *)(mem + LIST);
    p = emit_h(p, 0x11223344u, 0x250u, 0xFu);   /* voll schreiben */
    p = emit_h(p, 0xAABBCCDDu, 0x250u, 0x5u);   /* nur Bytes 0 und 2 */
    /* group=1, 2 Extra-Woerter: 0x240=Wert, 0x241/0x242 = Extra. */
    p[0] = 0x11111111u;
    p[1] = 0x240u | (0xFu << 16) | (2u << 20) | (1u << 31);
    p += 2;
    p[0] = 0x22222222u;
    p[1] = 0x33333333u;
    p += 2;
    /* group=0, 2 Extra-Woerter: 0x244 wird dreimal geschrieben. */
    p[0] = 0x00000001u;
    p[1] = 0x244u | (0xFu << 16) | (2u << 20);
    p += 2;
    p[0] = 0x00000002u;
    p[1] = 0x00000003u;
    p += 2;
    /* group=1, 1 Extra-Wort -> Record ist 3 Woerter lang (Pad-Wort). */
    p[0] = 0x00000064u;
    p[1] = 0x246u | (0xFu << 16) | (1u << 20) | (1u << 31);
    p += 2;
    *p++ = 0x000000C8u;   /* 0x247 */
    *p++ = 0;             /* Pad */
    p = emit_h(p, 0x0000CAFEu, 0x248u, 0xFu);
    size = (uint32_t)((unsigned char *)p - (mem + LIST));
    check(gpu_run_pica_list(&c, LIST, size) > 0, "Konfigurationsliste liefert ops>0");
    check(gpu_pica_reg(0x250) == 0x11BB33DDu,
          "Maske 0x5 schreibt nur Bytes 0 und 2");
    check(gpu_pica_reg(0x240) == 0x11111111u &&
          gpu_pica_reg(0x241) == 0x22222222u &&
          gpu_pica_reg(0x242) == 0x33333333u,
          "group=1 schreibt id, id+1, id+2");
    check(gpu_pica_reg(0x244) == 0x00000003u && gpu_pica_reg(0x245) == 0x00000000u,
          "group=0 schreibt dasselbe Register n-mal");
    check(gpu_pica_reg(0x246) == 0x00000064u && gpu_pica_reg(0x247) == 0x000000C8u,
          "group=1 mit 1 Extra-Wort (Pad-Wort)");
    check(gpu_pica_reg(0x248) == 0x0000CAFEu,
          "Record nach Pad-Wort wird korrekt gelesen");

    /* ---- 0x010 Finalize: P3D + irq_autostop ----
     * Azahar-Default irq_compare = 0x12345678, irq_autostop = 1. */
    gpu_pica_reset();
    irq_count = 0;
    gpu_pica_set_irq_handler(test_irq);
    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x020u, 0x12345678u);
    p = emit(p, 0x010u, 0x12345678u);   /* any_byte_match -> P3D */
    p = emit(p, 0x249u, 0xAAAAAAAAu);   /* nach Autostop nicht mehr ausfuehren */
    size = (uint32_t)((unsigned char *)p - (mem + LIST));
    gpu_run_pica_list(&c, LIST, size);
    check(irq_count == 1, "0x010 Finalize meldet P3D");
    check(gpu_pica_reg(0x010) == 0x12345678u, "0x010 enthaelt den geschriebenen Wert");
    check(gpu_pica_reg(0x249) == 0x00000000u,
          "0x010 mit irq_autostop stoppt die Liste");

    /* Kein Match: irq_request ohne gemeinsames Byte mit irq_compare. */
    gpu_pica_reset();
    irq_count = 0;
    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x010u, 0x00000000u);
    p = emit(p, 0x249u, 0x00000055u);
    size = (uint32_t)((unsigned char *)p - (mem + LIST));
    gpu_run_pica_list(&c, LIST, size);
    check(irq_count == 0, "0x010 ohne Byte-Treffer meldet keinen P3D");
    check(gpu_pica_reg(0x249) == 0x00000055u,
          "Liste laeuft ohne Irq-Treffer weiter");

    /* ---- Befehlspuffer-Sprung 0x238/0x23A/0x23C ----
     * Azahar GPU::SubmitCmdList/cmd_list.Reset: die neue Liste ersetzt die
     * alte, ihr Rest wird nicht ausgefuehrt. Adressen im Register sind
     * physisch; der Test benutzt eine bereits lineare Gastadresse. */
    gpu_pica_reset();
    irq_count = 0;
    {
        uint32_t *q = (uint32_t *)(mem + LIST2);
        uint32_t size2;
        int ops;
        q = emit_h(q, 0x5A5A5A5Au, 0x24Au, 0xFu);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST2));
        check(size2 == 8u, "Sprungliste ist 8 Byte lang");

        p = (uint32_t *)(mem + LIST);
        p = emit_h(p, 0x00000006u, 0x240u, 0xFu);
        p = emit_h(p, size2 / 8u, 0x238u, 0xFu);        /* CMDBUF_SIZE0 */
        p = emit_h(p, LIST2 / 8u, 0x23Au, 0xFu);        /* CMDBUF_ADDR0 */
        p = emit_h(p, 0u, 0x23Cu, 0xFu);                /* CMDBUF_JUMP0 = 0 */
        p = emit_h(p, 0x00000BADu, 0x241u, 0xFu);       /* darf nicht laufen */
        size = (uint32_t)((unsigned char *)p - (mem + LIST));
        ops = gpu_run_pica_list(&c, LIST, size);
        check(ops == 5, "Sprung zaehlt 4 aeussere + 1 innere Schreibung");
        check(gpu_pica_reg(0x24A) == 0x5A5A5A5Au, "Sprung bei Trigger-Wert 0 wird ausgefuehrt");
        check(gpu_pica_reg(0x23C) == 0x00000000u, "Trigger-Register behaelt den Wert 0");
        check(gpu_pica_reg(0x241) == 0x00000000u,
              "Rest der alten Liste nach dem Sprung wird nicht ausgefuehrt");
    }

    /* ---- Vertex-Shader: float24-Uniforms, Programm, Ausgangs-Mapping ---- */
    {
        uint32_t words[2], pack[3], *q, size2;
        float pos[4], col[4], uv[2];

        gpu_pica_reset();
        memset(mem + LIST, 0, 0x800);
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);                 /* vs_output_total = 1 */
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */        /* x..w -> POSITION_X..W */
        q = emit(q, 0x2C0, 0u);                /* Uniform-Index 0, float24 */
        f24_pack3(1.0f, 2.0f, 3.0f, 4.0f, pack);
        q = emit(q, 0x2C1, pack[0]);
        q = emit(q, 0x2C1, pack[1]);
        q = emit(q, 0x2C1, pack[2]);
        words[0] = ins_common(0x13u, 0u, 0x20u, 0u, 0u, 0u);  /* mov o0,c0 */
        words[1] = 0x22u << 26;                                /* end */
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 1u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        check(gpu_pica_vs_active() == 1, "VS-Programm ueber 0x2CB/0x2CC geladen");
        check(gpu_pica_vs_uniform(0, 0) == 1.0f && gpu_pica_vs_uniform(0, 1) == 2.0f &&
              gpu_pica_vs_uniform(0, 2) == 3.0f && gpu_pica_vs_uniform(0, 3) == 4.0f,
              "float24-Uniform c0 = (1,2,3,4) entpackt");
        check(gpu_pica_last_vertex(pos, col, uv) == 1, "VS-Draw liefert Vertex");
        check(pos[0] == 1.0f && pos[1] == 2.0f && pos[2] == 3.0f && pos[3] == 4.0f,
              "MOV o0,c0 + 0x050-Mapping => Position (1,2,3,4)");
        check(col[3] == 1.0f, "nicht gemappte COLOR-Komponente bleibt 1.0");
    }

    /* ---- VS: Attribute -> Eingangsregister (0x2BB), ADD mit Swizzle ---- */
    {
        uint32_t words[2], pack[3], *q, size2;
        float pos[4];

        gpu_pica_reset();
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2B9, 0u);                /* max_input_attribute_index = 0 */
        q = emit(q, 0x2BB, 0u);                /* Attribut 0 -> v0 */
        q = emit_attr0(q, VTX);
        { float vv[3]; vv[0] = 0.5f; vv[1] = 0.25f; vv[2] = 0.0f;
          memcpy(mem + VTX, vv, sizeof(vv)); }
        q = emit(q, 0x2C0, 0u);
        f24_pack3(1.0f, 2.0f, 3.0f, 4.0f, pack);
        q = emit(q, 0x2C1, pack[0]);
        q = emit(q, 0x2C1, pack[1]);
        q = emit(q, 0x2C1, pack[2]);
        words[0] = ins_common(0x00u, 0u, 0x20u, 0x00u, 0u, 0u);  /* add o0,c0,v0 */
        words[1] = 0x22u << 26;
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 1u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 1.5f && pos[1] == 2.25f && pos[2] == 3.0f && pos[3] == 5.0f,
              "ADD v0+c0 mit xyzw-Swizzle => (1.5,2.25,3,5)");
    }

    /* ---- VS: LOOP + aL-Adressierung (c[aL]) ---- */
    {
        uint32_t words[5], pack[3], *q, size2;
        float pos[4];
        int i;

        gpu_pica_reset();
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2B1, (2u) | (0u << 8) | (1u << 16));  /* i0: x=2,y=0,z=1 */
        q = emit(q, 0x2C0, 0u);
        for (i = 0; i < 4; i++) {
            float v = (i == 0) ? 1.0f : (i == 1) ? 2.0f : (i == 2) ? 4.0f : 0.0f;
            f24_pack3(v, v, v, v, pack);
            q = emit(q, 0x2C1, pack[0]);
            q = emit(q, 0x2C1, pack[1]);
            q = emit(q, 0x2C1, pack[2]);
        }
        words[0] = ins_common(0x13u, 0u, 0x23u, 0u, 0x10u, 0u);  /* mov r0,c3 */
        words[1] = ins_flow(0x29u, 0u, 2u, 0u, 0u, 0u);          /* loop i0 */
        words[2] = ins_common(0x00u, 0u, 0x20u, 0x10u, 0x10u, 3u);/* add r0,c[aL],r0 */
        words[3] = ins_common(0x13u, 0u, 0x10u, 0u, 0u, 0u);    /* mov o0,r0 */
        words[4] = 0x22u << 26;
        q = emit_program(q, words, 5);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 1u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 7.0f && pos[1] == 7.0f && pos[2] == 7.0f,
              "LOOP 3x ADD c[aL] (aL=0,1,2) => 1+2+4 = 7");
    }

    /* ---- VS: IFU/Bool-Uniform (0x2B0) ---- */
    {
        uint32_t words[4], pack[3], *q, size2;
        float pos[4];

        gpu_pica_reset();
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2C0, 0u);
        f24_pack3(1.0f, 0.0f, 0.0f, 0.0f, pack);
        q = emit(q, 0x2C1, pack[0]);
        q = emit(q, 0x2C1, pack[1]);
        q = emit(q, 0x2C1, pack[2]);
        f24_pack3(0.0f, 1.0f, 0.0f, 0.0f, pack);
        q = emit(q, 0x2C1, pack[0]);
        q = emit(q, 0x2C1, pack[1]);
        q = emit(q, 0x2C1, pack[2]);
        words[0] = ins_flow(0x27u, 1u, 2u, 0u, 0u, 0u);            /* ifu b0 */
        words[1] = ins_common(0x13u, 0u, 0x20u, 0u, 0u, 0u);       /* mov o0,c0 */
        words[2] = ins_common(0x13u, 0u, 0x21u, 0u, 0u, 0u);       /* mov o0,c1 */
        words[3] = 0x22u << 26;
        q = emit_program(q, words, 4);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 1u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 0.0f && pos[1] == 1.0f,
              "IFU b0=0 springt auf else (c1)");
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x2B0, 1u);                /* bool uniform b0 = 1 */
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 1.0f && pos[1] == 0.0f,
              "IFU b0=1 fuehrt den Rumpf aus (c0)");
    }

    /* ---- Index-Puffer 0x227 + 0x22F (u16) ---- */
    {
        uint32_t words[2], *q, size2;
        float pos[4];
        uint16_t idx[3];

        gpu_pica_reset();
        idx[0] = 2; idx[1] = 0; idx[2] = 1;
        memcpy(mem + VTX3 + 0x100, idx, sizeof(idx));
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2B9, 0u);
        q = emit(q, 0x2BB, 0u);
        q = emit_attr0(q, VTX3);
        { float vv[3][3] = {{-1.0f,-1.0f,0.0f},{1.0f,-1.0f,0.0f},{0.0f,1.0f,0.0f}};
          int vi; for (vi = 0; vi < 3; vi++) memcpy(mem + VTX3 + vi * 16, vv[vi], sizeof(vv[vi])); }
        words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);  /* mov o0,v0 */
        words[1] = 0x22u << 26;
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x227, 0x100u | (1u << 31));   /* Offset 0x100, u16 */
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22F, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 1.0f && pos[1] == -1.0f,
              "0x22F/u16-Index {2,0,1}: letzter Vertex ist v1 = (1,-1)");
    }

    /* ---- Default-Attribut ueber 0x232/0x233 + attribute_mask ---- */
    {
        uint32_t words[2], pack[3], *q, size2;
        float pos[4];

        gpu_pica_reset();
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2B9, 0u);
        q = emit(q, 0x2BB, 0u);
        words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);  /* mov o0,v0 */
        words[1] = 0x22u << 26;
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x232, 0u);                /* Default-Attribut-Index 0 */
        f24_pack3(0.5f, 0.25f, 0.0f, 1.0f, pack);
        q = emit(q, 0x233, pack[0]);
        q = emit(q, 0x233, pack[1]);
        q = emit(q, 0x233, pack[2]);
        q = emit(q, 0x200, VTX >> 3);
        q = emit(q, 0x201, 0xBu);
        q = emit(q, 0x202, (1u << 16) | 0u);   /* attr0 = Default */
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 0.5f && pos[1] == 0.25f,
              "attribute_mask/Default-Attribut 0x232 => POSITION (0.5,0.25)");
    }

    /* ---- Immediate-Mode 0x232=0xF: drei Attribute bilden einen Vertex ---- */
    {
        uint32_t words[2], pack[3], *q, size2;
        float pos[4];

        gpu_pica_reset();
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x4F, 1u);
        q = emit(q, 0x50, 0x03020100u);
        q = emit(q, 0x2BD, 1u);                /* output_mask: o0 */
        q = emit(q, 0x2B9, 0u);
        q = emit(q, 0x2BB, 0u);
        words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);
        words[1] = 0x22u << 26;
        q = emit_program(q, words, 2);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x242, 0u);                /* 1 Attribut je Vertex */
        q = emit(q, 0x232, 0xFu);              /* Immediate-Modus */
        f24_pack3(-1.0f, -1.0f, 0.0f, 1.0f, pack);
        q = emit(q, 0x233, pack[0]); q = emit(q, 0x233, pack[1]); q = emit(q, 0x233, pack[2]);
        f24_pack3(1.0f, -1.0f, 0.0f, 1.0f, pack);
        q = emit(q, 0x233, pack[0]); q = emit(q, 0x233, pack[1]); q = emit(q, 0x233, pack[2]);
        f24_pack3(0.0f, 1.0f, 0.0f, 1.0f, pack);
        q = emit(q, 0x233, pack[0]); q = emit(q, 0x233, pack[1]); q = emit(q, 0x233, pack[2]);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        gpu_pica_last_vertex(pos, NULL, NULL);
        check(pos[0] == 0.0f && pos[1] == 1.0f,
              "Immediate-Mode 0x232=0xF verarbeitet Vertex (0,1)");
    }

    /* ---- TEV-Konstantfarbe (Quelle Constant + Replace) ----
     * Azahar sw_rasterizer.cpp get_source(Source::Constant) liest
     * const_color des Stages; Replace gibt Quelle 1 unveraendert aus. */
    {
        uint32_t *q, size2;
        gpu_pica_reset();
        memset(mem + FB, 0, 400u * 240u * 3u);
        set_vertex(&c, 0, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
        set_vertex(&c, 20, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
        set_vertex(&c, 40, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x200, VTX >> 3);
        q = emit(q, 0x201, 0xBu);              /* attr0 float3 */
        q = emit(q, 0x202, 0u);
        q = emit(q, 0x203, 0u);
        q = emit(q, 0x204, 0u);
        q = emit(q, 0x205, (1u << 28) | (20u << 16));
        q = t_emit_vs_basic(q);
        q = emit_viewport(q, 400, 240);
        q = emit_tev0(q, 0xEu, 0xEu, 0xFF304050u);  /* Constant, (r,g,b,a)=50,40,30,255 */
        q = emit(q, 0x228, 3u);
        q = t_emit_fb(q, FB, 400, 240, 0u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        cx = t_fb_off(FB, 200, 120, 400, 240, 4);
        check(mem[cx + 3] == 0x50 && mem[cx + 2] == 0x40 && mem[cx + 1] == 0x30,
              "TEV Constant/Replace schreibt (0x50,0x40,0x30)");
    }

    /* ---- Interpolierte Vertexfarbe (VS o1 -> COLOR_RGBA senses) ----
     * Azahar: primary_color = perspektivisch interpolierte Vertexfarbe
     * (GetInterpolatedAttribute), danach u8 = round(f*255). */
    {
        uint32_t words[3], *q, size2;
        float v[6][4];
        int vi;
        gpu_pica_reset();
        memset(mem + FB, 0, 400u * 240u * 3u);
        set_vertex(&c, 0, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
        set_vertex(&c, 20, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
        set_vertex(&c, 40, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
        /* 3 Vertices, Stride 32: Position float4 + Farbe float4. */
        v[0][0] = -1.0f; v[0][1] = -1.0f; v[0][2] = 0.0f; v[0][3] = 1.0f;
        v[1][0] =  1.0f; v[1][1] = -1.0f; v[1][2] =      0.0f; v[1][3] = 1.0f;
        v[2][0] =  0.0f; v[2][1] =  1.0f; v[2][2] = 0.0f; v[2][3] = 1.0f;
        v[3][0] =  1.0f; v[3][1] =  0.0f; v[3][2] = 0.0f; v[3][3] = 1.0f;
        v[4][0] =  0.0f; v[4][1] =  1.0f; v[4][2] = 0.0f; v[4][3] = 1.0f;
        v[5][0] =  0.0f; v[5][1] =  0.0f; v[5][2] = 1.0f; v[5][3] = 1.0f;
        for (vi = 0; vi < 3; vi++)
            memcpy(mem + VTX3 + vi * 32, v[vi], 16),
            memcpy(mem + VTX3 + vi * 32 + 16, v[vi + 3], 16);
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x200, VTX3 >> 3);
        q = emit(q, 0x201, 0xFFu);             /* attr0+attr1 float4 */
        q = emit(q, 0x202, 1u << 28);          /* max_attribute_index = 1 */
        q = emit(q, 0x203, 0u);                /* Loader: attr0 bei +0 */
        q = emit(q, 0x204, 0x0010u);           /* comp0=attr0, comp1=attr1 */
        q = emit(q, 0x205, (2u << 28) | (32u << 16));
        q = emit(q, 0x2B9, 1u);                /* max_input_attr_index */
        q = emit(q, 0x2BB, 0x10u);             /* attr0->v0, attr1->v1 */
        q = emit(q, 0x4F, 2u);                 /* 2 Ausgaenge */
        q = emit(q, 0x50, 0x03020100u);        /* o0 -> POSITION xyzw */
        q = emit(q, 0x51, 0x0B0A0908u);        /* o1 -> COLOR_RGBA */
        q = emit(q, 0x2BD, 3u);                /* o0 + o1 */
        words[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);  /* mov o0,v0 */
        words[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);  /* mov o1,v1 */
        words[2] = 0x22u << 26;
        q = emit_program(q, words, 3);
        q = emit_swizzle0(q, swz_xyzw());
        q = emit(q, 0x228, 3u);
        q = emit_viewport(q, 400, 240);
        q = emit_tev0(q, 0u, 0u, 0u);          /* PrimaryColor, Replace */
        q = t_emit_fb(q, FB, 400, 240, 0u);
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        cx = t_fb_off(FB, 200, 120, 400, 240, 4);
        check(abs((int)mem[cx + 3] - 63) <= 2 &&
              abs((int)mem[cx + 2] - 64) <= 2 &&
              abs((int)mem[cx + 1] - 128) <= 2,
              "interpolierte Vertexfarbe in der Dreiecksmitte ~(63,64,128)");
        px = t_fb_off(FB, 399, 239, 400, 240, 4);
        check(mem[px + 3] == 0 && mem[px + 2] == 0 && mem[px + 1] == 0,
              "Ecke (399,239) bleibt ausserhalb (kein Vollflaeche-Fuellen)");
    }

    /* ---- Konfiguriertes Farbziel 0x11C..0x11E (RGBA8, Morton, unten->oben) ----
     * Azahar sw_framebuffer.cpp DrawPixel: y = height_raw - y, 8x8-Morton,
     * Offset = morton(x,y,bpp) + (y & ~7) * width * bpp. */
    {
        uint32_t *q, size2;
        uint32_t off2;
        gpu_pica_reset();
        memset(mem + 0x40000u, 0, 0x40000u);
        set_vertex(&c, 0, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
        set_vertex(&c, 20, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
        set_vertex(&c, 40, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);
        q = (uint32_t *)(mem + LIST);
        q = emit(q, 0x200, VTX >> 3);
        q = emit(q, 0x201, 0xBu);
        q = emit(q, 0x202, 0u);
        q = emit(q, 0x203, 0u);
        q = emit(q, 0x204, 0u);
        q = emit(q, 0x205, (1u << 28) | (20u << 16));
        q = t_emit_vs_basic(q);
        q = emit_viewport(q, 400, 240);
        q = emit_tev0(q, 0xEu, 0xEu, 0x10203040u);
        q = emit(q, 0x228, 3u);
        /* Farbziel: RGBA8, Adresse 0x40000, 400x240, Schreibmaske RGBA. */
        q = emit(q, 0x117, 0u);                /* color_format RGBA8 */
        q = emit(q, 0x11D, 0x40000u >> 3);     /* color_buffer_address */
        q = emit(q, 0x11E, 400u | ((240u - 1u) << 12));
        q = emit(q, 0x107, 0x00000F00u);       /* red..alpha enable */
        q = emit(q, 0x102, 3u);                /* LogicOp Copy */
        q = emit(q, 0x113, 1u);                /* allow_color_write */
        q = emit(q, 0x22E, 0u);
        size2 = (uint32_t)((unsigned char *)q - (mem + LIST));
        gpu_run_pica_list(&c, LIST, size2);
        /* Pixel (200,120) -> y'=119; Morton: y%8=7 -> 0x2A, x%8=0,
           coarse_x=200 -> +1600; *4 Byte; plus (119&~7)*400*4. */
        off2 = 0x40000u + (42u + 1600u) * 4u + 112u * 400u * 4u;
        /* Azahar EncodeRGBA8: bytes = a,b,g,r (Wort 0xRRGGBBAA). */
        check(mem[off2 + 3] == 0x40 && mem[off2 + 2] == 0x30 &&
              mem[off2 + 1] == 0x20 && mem[off2 + 0] == 0x10,
              "Farbziel 0x11D/0x11E: RGBA8 a,b,g,r + Morton-Pixel korrekt");
    }

    test_textures(&c, mem);
    test_texture_morton(&c, mem);
    test_texture_wrap(&c, mem);
    test_texture_filter(&c, mem);
    test_proctex_combiner(&c, mem);
    test_proctex_clamp_shift(&c, mem);
    test_proctex_noise(&c, mem);
    test_proctex_alpha_filter(&c, mem);
    test_proctex_lut_upload(&c, mem);
    test_proctex_coordinates(&c, mem);
    test_fb_formats(&c, mem);
    test_logic_ops(&c, mem);
    test_alpha_test(&c, mem);
    test_depth_stencil(&c, mem);
    test_texture_type_and_tc2(&c, mem);
    test_fog(&c, mem);
    test_blend(&c, mem);
    test_vs_entrypoint(&c, mem);
    test_vs_dphi(&c, mem);
    test_vs_inverted_ops(&c, mem);
    test_cull_keep_clockwise(&c, mem);
    test_triangle_strip(&c, mem);
    test_immediate_topology(&c, mem);
    test_etc1(&c, mem);
    test_texcache_etc1(&c, mem);
    test_uniform_masks(&c, mem);
    test_no_program(&c, mem);
    test_fb_clamp(&c, mem);
    test_lighting(&c, mem);
    test_lighting_review(&c, mem);
    test_texture_projection_cube(&c, mem);
    test_gs_point(&c, mem);
    test_gs_fixed_primitive(&c, mem);
    test_gs_variable_primitive(&c, mem);
    test_gs_exclusive_program_share(&c, mem);
    test_gs_winding(&c, mem);
    test_gs_output_bank(&c, mem);
    test_gs_immediate(&c, mem);

    printf("ERGEBNIS: %s\n", fails == 0 ? "OK" : "FEHLER");
    {
        const char *b = getenv("PICA_BENCH");
        if (b && *b && *b != '0') {
            if (pica_bench() != 0) fails++;
        }
    }
    return fails == 0 ? 0 : 1;
}
