/* Hardware-Renderer-Selbsttest (M9-Grundgeruest).
 *
 * Rendert dieselben synthetischen PICA-Kommandolisten einmal mit dem
 * Software-Rasterizer (runtime/gpu/gpu.c) und einmal mit dem D3D11-Pfad
 * (runtime/gpu/hw_d3d11.c, WARP oder Hardware) und vergleicht die
 * geschriebenen Gast-Framebuffer pixelweise.
 *
 * Toleranz: pro Farbkanal maximal 1 LSB (Rundung Float->UNORM8 gegen die
 * Byte-Rundung des Software-Pfads). Pixel mit groesserer Abweichung gelten
 * als Kanten-/Deckungspixel (Top-Left-Regel und Subpixel-Genauigkeit von
 * D3D gegen den 12.4-Fixed-Point-Rasterizer) und werden gezaehlt; je Szene
 * ist nur ein kleines Budget erlaubt.
 *
 * Aufruf ueber tests/hw_test.py (baut und startet diese Datei).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "arm_cpu.h"
#include "gpu.h"
#include "hw_d3d11.h"

uint32_t arm_last_block = 0;
void arm_trace_svc(uint32_t imm) { (void)imm; }

#define LIST   0x00001000u
#define VTX    0x00020000u
#define FB     0x00030000u
#define DEPTH  0x00038000u
#define TEX0   0x00050000u
#define TEX1   0x00051000u

static int fails = 0;
static void check(int cond, const char *whatprints) {
    printf("%s %s\n", cond ? "OK  " : "FEHL", whatprints);
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

static uint32_t f24raw(float f) {
    uint32_t u, sign, exp, mant;
    memcpy(&u, &f, 4);
    sign = (u >> 31) & 1u;
    exp = (u >> 23) & 0xFFu;
    mant = u & 0x7FFFFFu;
    if ((u & 0x7FFFFFFFu) == 0u) return 0u;
    if (exp == 0u || exp >= 255u) return 0u;
    exp = exp - 127u + 63u;
    if (exp >= 128u) return 0u;
    return (sign << 23) | ((exp & 0x7Fu) << 16) | (mant >> 7);
}

static uint32_t ins_common(uint32_t op, uint32_t desc, uint32_t src1,
                           uint32_t src2, uint32_t dest, uint32_t ari) {
    return (op << 26) | (desc & 0x7Fu) | ((src2 & 0x1Fu) << 7) |
           ((src1 & 0x7Fu) << 12) | ((ari & 3u) << 19) | ((dest & 0x1Fu) << 21);
}
static uint32_t swz_xyzw(void) {
    return 0xFu | (1u << 9) | (2u << 7) | (3u << 5) |
           (1u << 18) | (2u << 16) | (3u << 14);
}
static uint32_t *emit_program(uint32_t *p, const uint32_t *words, int n) {
    int i;
    p = emit(p, 0x2CB, 0);
    for (i = 0; i < n; i++) p = emit(p, 0x2CC, words[i]);
    return p;
}

/* Vertex-Shader: o0 = v0 (Position), o1 = v1 (Farbe). */
static uint32_t *emit_vs_real(uint32_t *q) {
    uint32_t w[3];
    q = emit(q, 0x2B9, 1u);
    q = emit(q, 0x2BB, 0x10u);             /* attr0->v0, attr1->v1 */
    q = emit(q, 0x4F, 2u);
    q = emit(q, 0x50, 0x03020100u);        /* o0 -> POSITION */
    q = emit(q, 0x51, 0x0B0A0908u);        /* o1 -> COLOR r,g,b,a */
    q = emit(q, 0x2BD, 3u);
    w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);  /* mov o0,v0 */
    w[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);  /* mov o1,v1 */
    w[2] = 0x22u << 26;                                 /* end */
    q = emit_program(q, w, 3);
    q = emit(q, 0x2D5, 0);
    return emit(q, 0x2D6, swz_xyzw());
}

static uint32_t *emit_viewport(uint32_t *q, int w, int h) {
    q = emit(q, 0x41, f24raw((float)w * 0.5f));
    q = emit(q, 0x43, f24raw((float)h * 0.5f));
    return emit(q, 0x68, 0u);
}

static uint32_t *emit_tev0(uint32_t *q, uint32_t src, uint32_t cst) {
    int s;
    q = emit(q, 0xC0, (src & 0xFu) | ((src & 0xFu) << 16));
    q = emit(q, 0xC1, 0u);
    q = emit(q, 0xC2, 0u);
    q = emit(q, 0xC3, cst);
    q = emit(q, 0xC4, 0u);
    for (s = 1; s < 6; s++) {
        uint32_t base = (s < 4) ? (0xC0u + (uint32_t)s * 8u)
                                : (0xF0u + (uint32_t)(s - 4) * 8u);
        q = emit(q, base + 0u, 0x000F000Fu);
        q = emit(q, base + 1u, 0u);
        q = emit(q, base + 2u, 0u);
        q = emit(q, base + 3u, 0u);
        q = emit(q, base + 4u, 0u);
    }
    return q;
}

typedef struct {
    float pos[3];
    float col[4];
} Vtx;

static void put_vertices(ArmCpu *c, const Vtx *v, int n) {
    int i;
    for (i = 0; i < n; i++) {
        memcpy(c->mem + ((VTX + (uint32_t)i * 28u) & 0x1FFFFFFFu), v[i].pos, 12);
        memcpy(c->mem + ((VTX + (uint32_t)i * 28u + 12u) & 0x1FFFFFFFu), v[i].col, 16);
    }
}

typedef struct {
    float pos[3];
    float col[4];
    float uv[2];
} Vtx3;

static void put_vertices3(ArmCpu *c, const Vtx3 *v, int n) {
    int i;
    for (i = 0; i < n; i++) {
        uint32_t a = VTX + (uint32_t)i * 36u;
        memcpy(c->mem + (a & 0x1FFFFFFFu), v[i].pos, 12);
        memcpy(c->mem + ((a + 12u) & 0x1FFFFFFFu), v[i].col, 16);
        memcpy(c->mem + ((a + 28u) & 0x1FFFFFFFu), v[i].uv, 8);
    }
}

/* Vertex-Shader mit Position/Farbe/UV; two_uv = zusaetzlich TEXCOORD1. */
static uint32_t *emit_vs_tex(uint32_t *q, int two_uv) {
    uint32_t w[5];
    q = emit(q, 0x2B9, 1u);
    q = emit(q, 0x2BB, 0x10u);
    q = emit(q, 0x4F, two_uv ? 4u : 3u);
    q = emit(q, 0x50, 0x03020100u);
    q = emit(q, 0x51, 0x0B0A0908u);
    q = emit(q, 0x52, 0x0D0Cu);
    q = emit(q, 0x2BD, two_uv ? 0xFu : 0x7u);
    w[0] = ins_common(0x13u, 0u, 0x00u, 0u, 0u, 0u);  /* mov o0,v0 */
    w[1] = ins_common(0x13u, 0u, 0x01u, 0u, 1u, 0u);  /* mov o1,v1 */
    w[2] = ins_common(0x13u, 0u, 0x02u, 0u, 2u, 0u);  /* mov o2,v2 */
    if (two_uv) {
        q = emit(q, 0x53, 0x0F0Eu);                    /* TEXCOORD1 */
        w[3] = ins_common(0x13u, 0u, 0x02u, 0u, 3u, 0u);
        w[4] = 0x22u << 26;
        q = emit_program(q, w, 5);
    } else {
        w[3] = 0x22u << 26;
        q = emit_program(q, w, 4);
    }
    q = emit(q, 0x2D5, 0);
    return emit(q, 0x2D6, swz_xyzw());
}

static uint32_t *emit_attrs(uint32_t *q, int nv) {
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0xFB);              /* attr0 float3, attr1 float4 */
    q = emit(q, 0x202, 1u << 28);          /* 2 Attribute */
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);
    q = emit(q, 0x205, (3u << 28) | (28u << 16));
    q = emit(q, 0x206, 12u);
    q = emit(q, 0x207, 1u);
    q = emit(q, 0x208, (1u << 28) | (28u << 16));
    return emit(q, 0x228, (uint32_t)nv);
}

/* Drei Attribute: float3 Position, float4 Farbe, float2 UV (Stride 36). */
static uint32_t *emit_attrs3(uint32_t *q, int nv) {
    q = emit(q, 0x200, VTX >> 3);
    q = emit(q, 0x201, 0xB | (0xFu << 4) | (0x7u << 8));
    q = emit(q, 0x202, 2u << 28);          /* 3 Attribute */
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);            /* attr0, attr1, attr2 */
    q = emit(q, 0x205, (3u << 28) | (36u << 16));
    return emit(q, 0x228, (uint32_t)nv);
}

/* Textur-Einheit 0..2 konfigurieren (8x8-Kacheln, Format wie 0x8E). */
static uint32_t *emit_tex(uint32_t *q, int unit, uint32_t addr, int w, int h,
                          uint32_t fmt, uint32_t wrap_s, uint32_t wrap_t,
                          uint32_t border, uint32_t mag_linear) {
    uint32_t base = (unit == 0) ? 0x81u : ((unit == 1) ? 0x91u : 0x99u);
    uint32_t fmtreg = (unit == 0) ? 0x8Eu : ((unit == 1) ? 0x96u : 0x9Eu);
    q = emit(q, base + 0u, border);
    q = emit(q, base + 1u, ((uint32_t)w << 16) | (uint32_t)h);
    q = emit(q, base + 2u, (wrap_t << 8) | (wrap_s << 12) | (mag_linear << 1));
    q = emit(q, base + 3u, 0u);
    q = emit(q, base + 4u, addr >> 3);
    return emit(q, fmtreg, fmt);
}

/* TEV-Stufe setzen; base 0xC0/0xC8/0xD0/0xD8/0xF0/0xF8. */
static uint32_t *emit_tev_stage(uint32_t *q, int s, uint32_t src, uint32_t mod,
                                uint32_t op, uint32_t cst, uint32_t scl) {
    uint32_t base = (s < 4) ? (0xC0u + (uint32_t)s * 8u)
                            : (0xF0u + (uint32_t)(s - 4) * 8u);
    q = emit(q, base + 0u, src);
    q = emit(q, base + 1u, mod);
    q = emit(q, base + 2u, op);
    q = emit(q, base + 3u, cst);
    return emit(q, base + 4u, scl);
}

/* Alle sechs Stufen auf Durchreichen von Previous setzen. */
static uint32_t *emit_tev_passthrough(uint32_t *q, int from) {
    int s;
    for (s = from; s < 6; s++) {
        q = emit_tev_stage(q, s, 0x000F000Fu, 0u, 0u, 0u, 0u);
    }
    return q;
}

/* RGBA8-Textur 8x8 (eine Kachel) im PICA-Morton-Layout fuellen.
 * r/g/b/a werden aus (x,y,seed) berechnet; Speicherreihenfolge a,b,g,r. */
static void fill_tex_rgba8(ArmCpu *c, uint32_t base, int seed) {
    static const int mx[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const int my[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    int x, y;
    for (y = 0; y < 8; y++) {
        for (x = 0; x < 8; x++) {
            unsigned char *p = c->mem + ((base + (uint32_t)(mx[x] + my[y]) * 4u)
                                         & 0x1FFFFFFFu);
            int rr = (x * 32 + seed * 8) & 0xFF;
            int gg = (y * 32 + seed * 16) & 0xFF;
            int bb = (x * 16 + y * 16 + seed * 32) & 0xFF;
            int aa = (seed == 3) ? ((x < 4) ? 40 : 240) : 255;
            p[0] = (unsigned char)aa;
            p[1] = (unsigned char)bb;
            p[2] = (unsigned char)gg;
            p[3] = (unsigned char)rr;
        }
    }
}

static uint32_t *emit_target(uint32_t *q, uint32_t addr, int w, int h,
                             uint32_t fmt, uint32_t depth_fmt, int depth,
                             uint32_t depth_func) {
    uint32_t dm = 0x00000F00u;             /* volle Kanalmaske */
    q = emit(q, 0x117, (fmt & 7u) << 16);
    q = emit(q, 0x11D, addr >> 3);
    q = emit(q, 0x11E, (uint32_t)w | ((uint32_t)(h - 1) << 12));
    q = emit(q, 0x6D, 1u);                 /* kein W-Buffer */
    q = emit(q, 0x4D, f24raw(-1.0f));      /* depth = -z/w */
    q = emit(q, 0x4E, 0u);
    if (depth) {
        dm |= 1u | (depth_func << 4) | (1u << 12);
        q = emit(q, 0x115, 1u);            /* allow_depth_stencil_write */
        q = emit(q, 0x11C, DEPTH >> 3);
        q = emit(q, 0x116, depth_fmt & 3u);
    }
    q = emit(q, 0x107, dm);
    q = emit(q, 0x102, 3u);                /* LogicOp Copy */
    q = emit(q, 0x113, 1u);
    q = emit(q, 0x100, 0u);                /* kein Blending */
    return emit(q, 0x1C6, 1u);             /* Fragment-Lighting aus */
}

static uint32_t scene_a(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.8f, -0.8f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.8f, -0.8f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.8f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tev0(q, 0xEu, 0xFF0000FFu);   /* konstantes Rot */
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

static uint32_t scene_b(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.85f, -0.85f, 0.0f}, {1, 0, 0, 1}},
        {{ 0.85f, -0.85f, 0.0f}, {0, 1, 0, 1}},
        {{ 0.0f,   0.85f, 0.0f}, {0, 0, 1, 1}},
    };
    uint32_t *q;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tev0(q, 0u, 0u);              /* PrimaryColor */
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

static uint32_t scene_c(ArmCpu *c) {
    static const Vtx v[6] = {
        /* fern: blau, z = -0.8 */
        {{-0.8f, -0.8f, -0.8f}, {0, 0, 1, 1}},
        {{ 0.8f, -0.8f, -0.8f}, {0, 0, 1, 1}},
        {{ 0.0f,  0.8f, -0.8f}, {0, 0, 1, 1}},
        /* nah: rot, z = -0.2 */
        {{-0.8f, -0.8f, -0.2f}, {1, 0, 0, 1}},
        {{ 0.8f, -0.8f, -0.2f}, {1, 0, 0, 1}},
        {{ 0.0f,  0.8f, -0.2f}, {1, 0, 0, 1}},
    };
    uint32_t *q;
    put_vertices(c, v, 6);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 6);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tev0(q, 0u, 0u);
    /* PICA-Vergleich 4 = LessThan, Schreiben an. */
    q = emit_target(q, FB, 16, 16, 0u, 0u, 1, 4u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

static uint32_t scene_d(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tev0(q, 0xEu, 0xFF20E020u);   /* konstantes Gruen (b=0x20,g=0xE0,r=0x20) */
    q = emit_target(q, FB, 16, 16, 3u, 0u, 0, 0u);  /* RGB565 */
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

static uint32_t scene_e(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-1.0f, -1.0f, 0.0f}, {1, 0, 1, 1}},
        {{ 1.0f, -1.0f, 0.0f}, {1, 0, 1, 1}},
        {{ 0.0f,  1.0f, 0.0f}, {1, 0, 1, 1}},
    };
    uint32_t *q;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tev0(q, 0xEu, 0xFFFF00FFu);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x65, 3u);                 /* Include */
    q = emit(q, 0x66, (0u << 16) | 8u);    /* x1=8, y1=0 */
    q = emit(q, 0x67, (15u << 16) | 15u);  /* x2=16, y2=16 */
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene F: RGBA8-Textur, Repeat, Nearest; TEV Stufe 0 Replace(Tex0),
 * Stufe 1 Modulate(Previous, PrimaryColor). */
static uint32_t scene_f(ArmCpu *c) {
    static const Vtx3 v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.0f, 0.0f}},
        {{ 0.9f, -0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.0f,  0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 1.0f}},
    };
    uint32_t *q;
    fill_tex_rgba8(c, TEX0, 1);
    put_vertices3(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs3(q, 3);
    q = emit_vs_tex(q, 0);
    q = emit_viewport(q, 16, 16);
    q = emit(q, 0x80, 1u);                 /* nur Einheit 0 */
    q = emit_tex(q, 0, TEX0, 8, 8, 0u, 2u, 2u, 0u, 0u);
    q = emit_tev_stage(q, 0, 0x00000003u, 0u, 0u, 0u, 0u);
    q = emit_tev_stage(q, 1, 0x0F0F0F0Fu, 0u, 0x00010001u, 0u, 0u);
    q = emit_tev_passthrough(q, 2);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene G: zwei Texturen mit Dot3_RGBA (Stufe 0), Alpha = Rot. */
static uint32_t scene_g(ArmCpu *c) {
    static const Vtx3 v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {0.0f, 0.0f}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {1.0f, 0.0f}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}, {0.5f, 1.0f}},
    };
    uint32_t *q;
    fill_tex_rgba8(c, TEX0, 1);
    fill_tex_rgba8(c, TEX1, 2);
    put_vertices3(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs3(q, 3);
    q = emit_vs_tex(q, 1);
    q = emit_viewport(q, 16, 16);
    q = emit(q, 0x80, 3u);                 /* Einheit 0 und 1 */
    q = emit_tex(q, 0, TEX0, 8, 8, 0u, 2u, 2u, 0u, 0u);
    q = emit_tex(q, 1, TEX1, 8, 8, 0u, 2u, 2u, 0u, 0u);
    q = emit_tev_stage(q, 0, 0x0F0FF43u, 0u, 0x00000007u, 0u, 0u);
    q = emit_tev_passthrough(q, 1);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene H: ClampToBorder + Alpha-Test (Greater 128). */
static uint32_t scene_h(ArmCpu *c) {
    static const Vtx3 v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {-0.25f, -0.25f}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {1.25f, -0.25f}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}, {0.5f, 1.25f}},
    };
    uint32_t *q;
    fill_tex_rgba8(c, TEX0, 3);
    put_vertices3(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs3(q, 3);
    q = emit_vs_tex(q, 0);
    q = emit_viewport(q, 16, 16);
    q = emit(q, 0x80, 1u);
    q = emit_tex(q, 0, TEX0, 8, 8, 0u, 1u, 1u, 0x8020E010u, 0u);
    q = emit_tev_stage(q, 0, 0x00000003u, 0u, 0u, 0u, 0u);
    q = emit_tev_passthrough(q, 1);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x104, 1u | (6u << 4) | (128u << 8));  /* Greater 128 */
    q = emit(q, 0x22E, 0);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene I: I8-Textur (8 Bit), eigener Dekodierpfad im Upload. */
static uint32_t scene_i(ArmCpu *c) {
    static const Vtx3 v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {0.0f, 0.0f}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}, {1.0f, 0.0f}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}, {0.0f, 1.0f}},
    };
    static const int mx[8] = {0, 1, 4, 5, 16, 17, 20, 21};
    static const int my[8] = {0, 2, 8, 10, 32, 34, 40, 42};
    uint32_t *q;
    int x, y;
    for (y = 0; y < 8; y++)
        for (x = 0; x < 8; x++)
            c->mem[(TEX0 + (uint32_t)(mx[x] + my[y])) & 0x1FFFFFFFu] =
                (unsigned char)((x * 20 + y * 7) & 0xFF);
    put_vertices3(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs3(q, 3);
    q = emit_vs_tex(q, 0);
    q = emit_viewport(q, 16, 16);
    q = emit(q, 0x80, 1u);
    q = emit_tex(q, 0, TEX0, 8, 8, 7u, 2u, 2u, 0u, 0u);
    q = emit_tev_stage(q, 0, 0x00000003u, 0u, 0u, 0u, 0u);
    q = emit_tev_passthrough(q, 1);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

static int bpp_of(uint32_t fmt) {
    switch (fmt) {
    case 1: return 3;
    case 2: case 3: case 4: return 2;
    default: return 4;
    }
}

typedef uint32_t (*SceneFn)(ArmCpu *c);

/* Rendert eine Szene mit dem gewaehlten Pfad und kopiert das Farbziel. */
static void render(ArmCpu *c, SceneFn fn, int hw, unsigned char *out,
                   int bytes, uint32_t fmt, int depth_fmt, int w, int h) {
    uint32_t size;
    gpu_pica_reset();
    gpu_pica_set_render_hw(hw);
    memset(c->mem + FB, 0, (size_t)w * h * (size_t)bpp_of(fmt));
    memset(c->mem + DEPTH, 0xFF, (size_t)w * h * 4u);
    size = fn(c);
    gpu_run_pica_list(c, LIST, size);
    memcpy(out, c->mem + FB, (size_t)bytes);
}

/* Vergleicht zwei Farbziele. Rueckgabe: Pixel mit Kanalabweichung > 1. */
static int compare_pixels(const unsigned char *a, const unsigned char *b,
                          int nbytes, int bpp, int *one_lsb, int *edge,
                          int *first_bad) {
    int i, bad = 0;
    *one_lsb = 0;
    *edge = 0;
    *first_bad = -1;
    for (i = 0; i + bpp <= nbytes; i += bpp) {
        int k, maxdiff = 0, anydiff = 0;
        for (k = 0; k < bpp; k++) {
            int d = (int)a[i + k] - (int)b[i + k];
            if (d < 0) d = -d;
            if (d > maxdiff) maxdiff = d;
            if (d != 0) anydiff = 1;
        }
        if (maxdiff > 1) {
            bad++;
            if (*first_bad < 0) *first_bad = i / bpp;
        }
        if (maxdiff == 1) (*one_lsb)++;
        if (anydiff && maxdiff > 1) (*edge)++;
    }
    return bad;
}

static void scene_case(ArmCpu *c, const char *name, SceneFn fn, uint32_t fmt,
                       int depth_fmt, int w, int h, int max_edge) {
    static unsigned char sw[64 * 64 * 4], hw[64 * 64 * 4];
    int bytes = w * h * bpp_of(fmt);
    int one_lsb, edge, first_bad, bad;
    char msg[160];

    render(c, fn, 0, sw, bytes, fmt, depth_fmt, w, h);
    render(c, fn, 1, hw, bytes, fmt, depth_fmt, w, h);
    bad = compare_pixels(sw, hw, bytes, bpp_of(fmt), &one_lsb, &edge, &first_bad);
    if (bad > 0)
        printf("     %s: %d Pixel >1 LSB (erster bei %d), %d Pixel 1 LSB, %d Kantenpixel\n",
               name, bad, first_bad, one_lsb, edge);
    else
        printf("     %s: identisch/<=1 LSB (%d Pixel 1 LSB, %d Kantenpixel)\n",
               name, one_lsb, edge);
    snprintf(msg, sizeof(msg), "%s: Software vs. D3D11 (%s), hoechstens %d Kantenpixel",
             name, hw_driver_name(), max_edge);
    check(bad <= max_edge, msg);
}

int main(int argc, char **argv) {
    static unsigned char mem[0x100000];
    ArmCpu c;
    int i;

    memset(&c, 0, sizeof(c));
    memset(mem, 0, sizeof(mem));
    arm_cpu_init(&c, mem, sizeof(mem));
    gpu_pica_set_raster_threads(0);

    if (!hw_init(argc > 1 && strcmp(argv[1], "--warp") == 0 ? "warp" : NULL)) {
        printf("FEHL D3D11-Device konnte nicht erzeugt werden\n");
        return 1;
    }
    printf("D3D11-Treiber: %s\n", hw_driver_name());

    /* Referenzlauf Software, dann Hardware; Szenen decken Viewport,
       konstante/vertexgefaerbte Dreiecke, Tiefentest, RGB565 und Scissor ab. */
    scene_case(&c, "Szene A: konstantes Rot, RGBA8", scene_a, 0u, 0, 16, 16, 8);
    scene_case(&c, "Szene B: Vertexfarben, RGBA8", scene_b, 0u, 0, 16, 16, 12);
    scene_case(&c, "Szene C: Tiefentest, RGBA8", scene_c, 0u, 0, 16, 16, 12);
    scene_case(&c, "Szene D: konstantes Gruen, RGB565", scene_d, 3u, 0, 16, 16, 8);
    scene_case(&c, "Szene E: Scissor Include, RGBA8", scene_e, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene F: Textur RGBA8 modulate, RGBA8", scene_f, 0u, 0, 16, 16, 32);
    scene_case(&c, "Szene G: zwei Texturen Dot3, RGBA8", scene_g, 0u, 0, 16, 16, 32);
    scene_case(&c, "Szene H: ClampToBorder + Alpha-Test, RGBA8", scene_h, 0u, 0, 16, 16, 48);
    scene_case(&c, "Szene I: I8-Textur, RGBA8", scene_i, 0u, 0, 16, 16, 32);

    /* Tiefenpuffer muss nach dem Hardware-Draw geschrieben sein. */
    {
        unsigned char swd[16 * 16 * 4], hwd[16 * 16 * 4];
        int nz_sw = 0, nz_hw = 0, k;
        render(&c, scene_c, 0, swd, 16 * 16 * 4, 0u, 0, 16, 16);
        memcpy(swd, c.mem + DEPTH, sizeof(swd));
        render(&c, scene_c, 1, hwd, 16 * 16 * 4, 0u, 0, 16, 16);
        memcpy(hwd, c.mem + DEPTH, sizeof(hwd));
        for (k = 0; k < 16 * 16 * 4; k++) {
            if (swd[k] != 0xFF) nz_sw++;
            if (hwd[k] != 0xFF) nz_hw++;
        }
        printf("     Tiefenpuffer: Software %d Bytes geschrieben, Hardware %d\n",
               nz_sw, nz_hw);
        check(nz_hw > 0, "Hardware-Pfad schreibt den Tiefenpuffer zurueck");
    }

    {
        unsigned long long nd = 0, nf = 0;
        hw_stats(&nd, &nf);
        printf("     Hardware-Draws=%llu Flushes=%llu Textur-Uploads=%llu\n",
               nd, nf, hw_texture_uploads());
        check(nd > 0, "Hardware-Pfad hat Draws ausgefuehrt");
        check(hw_texture_uploads() > 0, "Hardware-Pfad laedt Texturen hoch");
    }
    hw_shutdown();
    printf("%s (%d Fehler)\n", fails == 0 ? "HW-SELFTEST OK" : "HW-SELFTEST FEHLER", fails);
    return fails == 0 ? 0 : 1;
}
