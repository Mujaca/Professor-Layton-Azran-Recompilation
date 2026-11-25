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
#include <time.h>

#include "arm_cpu.h"
#include "gpu.h"
#include "hw_d3d11.h"

uint32_t arm_last_block = 0;
void arm_trace_svc(uint32_t imm) { (void)imm; }

#define LIST   0x00001000u
#define VTX    0x00020000u
#define FB     0x00030000u
#define DEPTH  0x00038000u
#define RTTDST 0x00040000u
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

static void put_vertices_at(ArmCpu *c, uint32_t base, const Vtx *v, int n) {
    int i;
    for (i = 0; i < n; i++) {
        memcpy(c->mem + ((base + (uint32_t)i * 28u) & 0x1FFFFFFFu), v[i].pos, 12);
        memcpy(c->mem + ((base + (uint32_t)i * 28u + 12u) & 0x1FFFFFFFu), v[i].col, 16);
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

static void put_vertices3_at(ArmCpu *c, uint32_t base, const Vtx3 *v, int n) {
    int i;
    for (i = 0; i < n; i++) {
        uint32_t a = base + (uint32_t)i * 36u;
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
static uint32_t *emit_attrs3_at(uint32_t *q, uint32_t base, int nv) {
    q = emit(q, 0x200, base >> 3);
    q = emit(q, 0x201, 0xB | (0xFu << 4) | (0x7u << 8));
    q = emit(q, 0x202, 2u << 28);          /* 3 Attribute */
    q = emit(q, 0x203, 0u);
    q = emit(q, 0x204, 0x210u);            /* attr0, attr1, attr2 */
    q = emit(q, 0x205, (3u << 28) | (36u << 16));
    return emit(q, 0x228, (uint32_t)nv);
}

static uint32_t *emit_attrs3(uint32_t *q, int nv) {
    return emit_attrs3_at(q, VTX, nv);
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
 * Stufe 1 Modulate(Previous, PrimaryColor). scene_tex_cmd emittiert nur die
 * Kommandoliste, damit ein Test die Gasttextur zwischen zwei Listen aendern
 * kann. */
static uint32_t scene_tex_cmd(ArmCpu *c) {
    static const Vtx3 v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.0f, 0.0f}},
        {{ 0.9f, -0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {1.0f, 0.0f}},
        {{ 0.0f,  0.9f, 0.0f}, {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 1.0f}},
    };
    uint32_t *q;
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

static uint32_t scene_f(ArmCpu *c) {
    fill_tex_rgba8(c, TEX0, 1);
    return scene_tex_cmd(c);
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

/* --- Szenen J..O: Output-Merger, Stencil, Fog ------------------------- */

static uint32_t morton_off(int x, int y, uint32_t bpp) {
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    uint32_t i = xlut[x & 7] + ylut[y & 7];
    return (i + (uint32_t)(x & ~7) * 8u) * bpp;
}

/* Gast-Farbziel (RGBA8, PICA-Kachelung) mit einem Muster vorbelegen. */
static void fill_fb_rgba8(ArmCpu *c, uint32_t base, int w, int h) {
    int x, y;
    for (y = 0; y < h; y++) {
        int yp = h - 1 - y;
        for (x = 0; x < w; x++) {
            uint32_t off = morton_off(x, yp, 4u) + (uint32_t)(yp & ~7) * (uint32_t)w * 4u;
            unsigned char *p = c->mem + ((base + off) & 0x1FFFFFFFu);
            p[0] = (unsigned char)(40 + x * 10);       /* a */
            p[1] = (unsigned char)(200 - y * 8);       /* b */
            p[2] = (unsigned char)(x * 12 + y * 4);    /* g */
            p[3] = (unsigned char)(255 - x * 9);       /* r */
        }
    }
}

static uint32_t *emit_blend(uint32_t *q, uint32_t enable, uint32_t eq_rgb,
                            uint32_t eq_a, uint32_t src_rgb, uint32_t dst_rgb,
                            uint32_t src_a, uint32_t dst_a, uint32_t cst) {
    q = emit(q, 0x100, enable ? (1u << 8) : 0u);
    q = emit(q, 0x101, (eq_rgb & 7u) | ((eq_a & 7u) << 8) |
                        ((src_rgb & 0xFu) << 16) | ((dst_rgb & 0xFu) << 20) |
                        ((src_a & 0xFu) << 24) | ((dst_a & 0xFu) << 28));
    return emit(q, 0x103, cst);
}

/* Ein Dreieck, konstante TEV-Farbe. z je Vertex fuer Fog/Depth. */
static uint32_t *emit_tri_color(uint32_t *q, uint32_t cst, float z) {
    Vtx v[3];
    v[0].pos[0] = -0.9f; v[0].pos[1] = -0.9f; v[0].pos[2] = z;
    v[1].pos[0] = 0.9f;  v[1].pos[1] = -0.9f; v[1].pos[2] = z;
    v[2].pos[0] = 0.0f;  v[2].pos[1] = 0.9f;  v[2].pos[2] = z;
    q = emit_tev0(q, 0xEu, cst);
    return q;
}

/* Szene J: Alpha-Blending SrcAlpha/OneMinusSrcAlpha (nativ). */
static uint32_t scene_j(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    fill_fb_rgba8(c, FB, 16, 16);
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0x60C08040u, 0.0f);   /* a=0x60 b=0xC0 g=0x80 r=0x40 */
    q = emit_blend(q, 1u, 0u, 0u, 6u, 7u, 6u, 7u, 0u);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene K: LogicOp Xor im Pixelshader. */
static uint32_t scene_k(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    fill_fb_rgba8(c, FB, 16, 16);
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0x80706050u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x102, 11u);                    /* Xor */
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene L: Farb-Schreibmaske nur Rot+Alpha. */
static uint32_t scene_l(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    fill_fb_rgba8(c, FB, 16, 16);
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF20E010u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x107, (1u << 8) | (1u << 11));  /* nur R + A */
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene M: Stencil Equal 0x0F in D24S8, nur linke Haelfte passend. */
static uint32_t scene_m(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    int x, y;
    for (y = 0; y < 16; y++) {
        int yp = 15 - y;
        for (x = 0; x < 16; x++) {
            uint32_t off = morton_off(x, yp, 4u) + (uint32_t)(yp & ~7) * 16u * 4u;
            c->mem[((DEPTH + off + 3u) & 0x1FFFFFFFu)] =
                (unsigned char)(x < 8 ? 0x0F : 0x00);
        }
    }
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF20E010u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 3u, 0, 0u);
    q = emit(q, 0x116, 3u);                     /* D24S8 */
    q = emit(q, 0x11C, DEPTH >> 3);
    q = emit(q, 0x115, 3u);                     /* Schreiben erlaubt */
    q = emit(q, 0x105, 1u | (2u << 4) | (0xFFu << 8) | (0x0Fu << 16) | (0xFFu << 24));
    q = emit(q, 0x106, 2u << 8);                /* Pass -> Replace */
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene N: Fog (Mode 5) mit 128 LUT-Eintraegen (value 0..0x7FF Rampe). */
static uint32_t scene_n(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, -1.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, -0.5f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    int i;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF20E010u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0xE0, 5u);                      /* Fog, kein Flip */
    q = emit(q, 0xE1, 0x00204080u);             /* b,g,r */
    for (i = 0; i < 128; i++)
        q = emit(q, 0xE8, ((((uint32_t)i * 8u) & 0x7FFu) << 13) | 8u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene O: Min-Blending (Shader-Pfad) und Konstantfarbe+Konstantalpha. */
static uint32_t scene_o(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.9f, -0.9f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.0f,  0.9f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    fill_fb_rgba8(c, FB, 16, 16);
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0x60C08040u, 0.0f);
    q = emit_blend(q, 1u, 3u, 3u, 10u, 12u, 10u, 12u, 0x20304050u);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene P: zwei Draws in derselben Liste, Texturadresse wechselt. */
static uint32_t scene_p(ArmCpu *c) {
    static const Vtx3 v[6] = {
        {{-0.95f, -0.9f, 0.0f}, {1, 1, 1, 1}, {0.0f, 0.0f}},
        {{-0.05f, -0.9f, 0.0f}, {1, 1, 1, 1}, {1.0f, 0.0f}},
        {{-0.50f,  0.9f, 0.0f}, {1, 1, 1, 1}, {0.5f, 1.0f}},
        {{ 0.05f, -0.9f, 0.0f}, {1, 1, 1, 1}, {0.0f, 0.0f}},
        {{ 0.95f, -0.9f, 0.0f}, {1, 1, 1, 1}, {1.0f, 0.0f}},
        {{ 0.50f,  0.9f, 0.0f}, {1, 1, 1, 1}, {0.5f, 1.0f}},
    };
    uint32_t *q;
    fill_tex_rgba8(c, TEX0, 1);
    fill_tex_rgba8(c, TEX1, 2);
    put_vertices3(c, v, 6);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs3(q, 3);
    q = emit_vs_tex(q, 0);
    q = emit_viewport(q, 16, 16);
    q = emit(q, 0x80, 1u);
    q = emit_tex(q, 0, TEX0, 8, 8, 0u, 2u, 2u, 0u, 0u);
    q = emit_tev_stage(q, 0, 0x00030003u, 0u, 0u, 0u, 0u);
    q = emit_tev_passthrough(q, 1);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    q = emit(q, 0x25E, 0u);
    q = emit_tex(q, 0, TEX1, 8, 8, 0u, 2u, 2u, 0u, 0u);
    q = emit(q, 0x200, (VTX + 3u * 36u) >> 3);
    q = emit(q, 0x228, 3u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene Q: D16-Tiefenziel im Gast vorbelegt (links nah, rechts fern). */
static uint32_t scene_q(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.95f, -0.95f, -0.5f}, {1, 1, 1, 1}},
        {{ 0.95f, -0.95f, -0.5f}, {1, 1, 1, 1}},
        {{ 0.00f,  0.95f, -0.5f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    int x, y;
    for (y = 0; y < 16; y++) {
        int yp = 15 - y;
        for (x = 0; x < 16; x++) {
            uint32_t off = morton_off(x, yp, 2u) + (uint32_t)(yp & ~7) * 16u * 2u;
            uint16_t z = (x < 8) ? 0x0000u : 0xFFFFu;
            memcpy(c->mem + ((DEPTH + off) & 0x1FFFFFFFu), &z, 2);
        }
    }
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF0000FFu, -0.5f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 1, 4u);
    q = emit(q, 0x116, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene R: D24-Ziel gebunden, aber ohne Tiefen-/Stencil-Schreiben. */
static uint32_t scene_r(ArmCpu *c) {
    static const Vtx v[3] = {
        {{-0.95f, -0.95f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.95f, -0.95f, 0.0f}, {1, 1, 1, 1}},
        {{ 0.00f,  0.95f, 0.0f}, {1, 1, 1, 1}},
    };
    uint32_t *q;
    int x, y;
    for (y = 0; y < 16; y++) {
        int yp = 15 - y;
        for (x = 0; x < 16; x++) {
            uint32_t off = morton_off(x, yp, 3u) + (uint32_t)(yp & ~7) * 16u * 3u;
            unsigned char *p = c->mem + ((DEPTH + off) & 0x1FFFFFFFu);
            p[0] = (unsigned char)(x * 13 + y);
            p[1] = (unsigned char)(x * 5 + y * 11);
            p[2] = (unsigned char)(x * 3 + y * 7);
        }
    }
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF20E010u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x11C, DEPTH >> 3);
    q = emit(q, 0x116, 2u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene T: eine Haelfte (links/rechts) mit konstanter Farbe. Wird fuer den
 * Test "CPU-Schreibzugriff zwischen zwei Listen" zweimal aufgerufen. */
static uint32_t scene_half(ArmCpu *c, int right) {
    Vtx v[3];
    uint32_t *q;
    if (right) {
        v[0].pos[0] = 0.1f;  v[0].pos[1] = -0.9f;
        v[1].pos[0] = 0.9f;  v[1].pos[1] = -0.9f;
        v[2].pos[0] = 0.5f;  v[2].pos[1] =  0.9f;
    } else {
        v[0].pos[0] = -0.9f; v[0].pos[1] = -0.9f;
        v[1].pos[0] =  0.0f; v[1].pos[1] = -0.9f;
        v[2].pos[0] = -0.45f; v[2].pos[1] = 0.9f;
    }
    v[0].pos[2] = v[1].pos[2] = v[2].pos[2] = 0.0f;
    v[0].col[0] = v[0].col[1] = v[0].col[2] = v[0].col[3] = 1.0f;
    v[1].col[0] = v[1].col[1] = v[1].col[2] = v[1].col[3] = 1.0f;
    v[2].col[0] = v[2].col[1] = v[2].col[2] = v[2].col[3] = 1.0f;
    put_vertices(c, v, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_tri_color(q, 0xFF20E010u, 0.0f);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    q = emit(q, 0x22E, 0u);
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

/* Szene U: vier Draws in einer Liste, HW/SW/HW/SW. Der Software-Fallback
 * wird ueber Fragment-Lighting (0x1C6 Bit0 = 0) erzwungen. */
static uint32_t scene_u(ArmCpu *c) {
    Vtx v[12];
    uint32_t *q;
    int i, k;
    for (i = 0; i < 4; i++) {
        float cx = -0.6f + 0.4f * (float)i;
        k = i * 3;
        v[k + 0].pos[0] = cx - 0.15f; v[k + 0].pos[1] = -0.9f;
        v[k + 1].pos[0] = cx + 0.15f; v[k + 1].pos[1] = -0.9f;
        v[k + 2].pos[0] = cx;         v[k + 2].pos[1] = -0.3f;
        v[k + 0].pos[2] = v[k + 1].pos[2] = v[k + 2].pos[2] = 0.0f;
        v[k + 0].col[0] = v[k + 0].col[3] = 1.0f;
        v[k + 1].col[1] = v[k + 1].col[3] = 1.0f;
        v[k + 2].col[2] = v[k + 2].col[3] = 1.0f;
    }
    for (i = 0; i < 4; i++) put_vertices_at(c, VTX + (uint32_t)i * 0x100u,
                                            v + i * 3, 3);
    q = (uint32_t *)(c->mem + LIST);
    q = emit_attrs(q, 3);
    q = emit_vs_real(q);
    q = emit_viewport(q, 16, 16);
    q = emit_target(q, FB, 16, 16, 0u, 0u, 0, 0u);
    for (i = 0; i < 4; i++) {
        q = emit(q, 0x1C6, (i & 1) ? 0u : 1u);   /* 0 = Lighting -> SW */
        q = emit_tri_color(q, 0xFFu - (uint32_t)i * 0x30u, 0.0f);
        /* Eigener 16-Byte-Slot je Dreieck (0x200 = addr>>3, Bit0 ignoriert). */
        q = emit(q, 0x200, (VTX + (uint32_t)i * 0x100u) >> 3);
        q = emit(q, 0x228, 3u);
        q = emit(q, 0x22E, 0u);
    }
    q = emit(q, 0x1C6, 1u);                /* Zustand fuer Folgetests */
    return (uint32_t)((unsigned char *)q - (c->mem + LIST));
}

#define FILL   0x00060000u

/* Minimaler HwDrawState fuer direkte API-Tests (TEV konstant, kein Tiefen-
 * ziel, volle Farbmaske, LogicOp Copy). */
static void hw_test_state(HwDrawState *st, ArmCpu *c, uint32_t fb, uint32_t cst) {
    int i;
    memset(st, 0, sizeof(*st));
    st->color_mem = c->mem + fb;
    st->color_size = (uint32_t)c->mem_size - fb;
    st->color_width = 16;
    st->color_height = 16;
    st->color_format = 0u;
    st->viewport_half_w = 8.0f;
    st->viewport_half_h = 8.0f;
    st->depth_range = -1.0f;
    st->logic_op = 3u;
    st->color_write_enable = 1;
    st->color_write_mask = 0xFu;
    for (i = 0; i < 6; i++) st->tev_src[i] = 0x000F000Fu;
    st->tev_src[0] = 0x000E000Eu;
    st->tev_cst[0] = cst;
    st->guest_mem = c->mem;
    st->guest_size = (uint32_t)c->mem_size;
    st->tex_generation = gpu_pica_tex_generation();
}

static const HwVertex g_hw_tri[3] = {
    {{-0.9f, -0.9f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
    {{ 0.9f, -0.9f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
    {{ 0.0f,  0.9f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
};

/* Voellig ausserhalb des Clip-Volumens: setzt depth_dirty, ohne Pixel. */
static const HwVertex g_hw_off[3] = {
    {{2.0f, 2.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
    {{3.0f, 2.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
    {{2.5f, 3.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0, 0}, {0, 0}, {0, 0}}},
};

static int any_nonzero(const unsigned char *p, int n) {
    int i;
    for (i = 0; i < n; i++) if (p[i]) return 1;
    return 0;
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
    if (bad > 0) {
        int idx, shown = 0;
        for (idx = 0; idx + bpp_of(fmt) <= bytes && shown < 6; idx += bpp_of(fmt)) {
            int k, maxdiff = 0;
            for (k = 0; k < bpp_of(fmt); k++) {
                int d = (int)sw[idx + k] - (int)hw[idx + k];
                if (d < 0) d = -d;
                if (d > maxdiff) maxdiff = d;
            }
            if (maxdiff > 1) {
                printf("       px#%d sw=%02X%02X%02X%02X hw=%02X%02X%02X%02X\n",
                       idx / bpp_of(fmt),
                       sw[idx], sw[idx + 1], sw[idx + 2], sw[idx + 3],
                       hw[idx], hw[idx + 1], hw[idx + 2], hw[idx + 3]);
                shown++;
            }
        }
    }
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
    scene_case(&c, "Szene J: Alpha-Blending, RGBA8", scene_j, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene K: LogicOp Xor, RGBA8", scene_k, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene L: Farbmaske R+A, RGBA8", scene_l, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene M: Stencil Equal, D24S8", scene_m, 0u, 3, 16, 16, 16);
    scene_case(&c, "Szene N: Fog, RGBA8", scene_n, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene O: Min + Konstantfarben-Blend", scene_o, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene P: Texturwechsel in einer Liste", scene_p, 0u, 0, 16, 16, 32);
    scene_case(&c, "Szene Q: Tiefe aus dem Gast geladen", scene_q, 0u, 0, 16, 16, 16);
    scene_case(&c, "Szene R: Tiefenziel ohne Schreiben", scene_r, 0u, 2, 16, 16, 16);

    /* Stencil-Bytes muessen wie im Software-Pfad geschrieben werden. */
    {
        static unsigned char sd[16 * 16 * 4], hd[16 * 16 * 4], col[16 * 16 * 4];
        int diff = 0, k;
        render(&c, scene_m, 0, col, sizeof(col), 0u, 3, 16, 16);
        memcpy(sd, c.mem + DEPTH, sizeof(sd));
        render(&c, scene_m, 1, col, sizeof(col), 0u, 3, 16, 16);
        memcpy(hd, c.mem + DEPTH, sizeof(hd));
        for (k = 0; k < 16 * 16 * 4; k += 4)
            if (sd[k + 3] != hd[k + 3]) diff++;
        printf("     Stencil-Bytes: %d Abweichungen Software/Hardware\n", diff);
        check(diff == 0, "Stencil-Rueckschreiben identisch zum Software-Pfad");
    }

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

    /* --- Review-Faelle: Zielerzeugung, Invalidierung, Readback, Cache ---- */
    {
        HwDrawState st, bad;
        unsigned long long d0 = 0, f0 = 0, d1 = 0, f1 = 0;
        unsigned long long u0, u1, g0, g1, dw0, dw1;
        uint32_t fw[8];
        int size;
        static unsigned char before[16 * 16 * 4];

        /* (1) create_targets scheitert (Breite 0): kein Zustand darf
           stehenbleiben, hw_flush_to_guest darf nicht auf NULL zeigen. */
        hw_test_state(&bad, &c, FB, 0xFF0000FFu);
        bad.color_width = 0u;
        check(hw_begin_draw(&bad) == 0, "create_targets-Fehler wird gemeldet");
        check(hw_flush_to_guest() == 0, "Flush nach fehlgeschlagenem Ziel ist sicher");

        /* (2a) Vor einer GX-Operation muessen ausstehende HW-Ergebnisse ins
           Gast-VRAM geschrieben werden. */
        memset(c.mem + FB, 0, 16 * 16 * 4);
        memset(c.mem + FILL, 0, 0x200);
        hw_test_state(&st, &c, FB, 0xFF0000FFu);
        check(hw_begin_draw(&st) != 0, "HW-Ziel fuer direkten Draw");
        hw_triangles(g_hw_tri, 3);
        memset(fw, 0, sizeof(fw));
        fw[1] = FILL; fw[2] = 0x11223344u; fw[3] = FILL + 0x100u; fw[7] = 0x101u;
        gpu_gsp_memory_fill_cmd(&c, fw);
        check(any_nonzero(c.mem + FB, 16 * 16 * 4),
              "GX-MemoryFill sichert vorher ausstehende HW-Ergebnisse");
        check(c.mem[FILL] == 0x44u && c.mem[FILL + 1] == 0x33u,
              "GX-MemoryFill schreibt danach den Gastpuffer");

        /* (2b) Hat der Gast selbst geschrieben, darf der veraltete GPU-Stand
           nicht zurueckgeschrieben werden (CPU-/Y2R-Schreibzugriff). */
        memset(c.mem + FB, 0, 16 * 16 * 4);
        check(hw_begin_draw(&st) != 0, "HW-Ziel erneut gebunden");
        hw_triangles(g_hw_tri, 3);
        memset(c.mem + FB, 0x5A, 16 * 16 * 4);
        hw_invalidate_guest();
        check(hw_flush_to_guest() == 0, "nach Gast-Schreibzugriff kein Readback");
        check(c.mem[FB] == 0x5Au && c.mem[FB + 100] == 0x5Au,
              "Gast-Schreibzugriff bleibt unveraendert");

        /* (5) HW-Rueckschreiben erhoeht die Texturgeneration. */
        g0 = gpu_pica_tex_generation();
        check(hw_begin_draw(&st) != 0, "HW-Ziel fuer Generationstest");
        hw_triangles(g_hw_tri, 3);
        hw_flush_to_guest();
        g1 = gpu_pica_tex_generation();
        check(g1 > g0, "HW-Rueckschreiben erhoeht die Texturgeneration");

        /* (6) Zwei Draws in einer Liste: genau ein Readback am Listenende
           (kein vollstaendiger GPU->CPU-Transfer je Draw). */
        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        size = (int)scene_p(&c);
        hw_stats(&d0, &f0);
        u0 = hw_texture_uploads();
        gpu_run_pica_list(&c, LIST, (uint32_t)size);
        hw_stats(&d1, &f1);
        u1 = hw_texture_uploads();
        check(d1 - d0 >= 2, "Szene P fuehrt zwei Draws aus");
        check(f1 - f0 <= 1, "hoechstens ein Readback je Kommandoliste");
        check(u1 - u0 <= 2, "Texturcache dekodiert je Draw hoechstens eine neue Textur");
        /* (3b) Ohne Tiefen-/Stencil-Schreiben bleibt das Gast-Tiefenziel
           unangetastet (kein D24-Roundtrip, kein Readback). */
        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        size = (int)scene_r(&c);
        memcpy(before, c.mem + DEPTH, sizeof(before));
        dw0 = hw_depth_writes();
        gpu_run_pica_list(&c, LIST, (uint32_t)size);
        dw1 = hw_depth_writes();
        check(dw1 == dw0, "kein Tiefen-Readback ohne Tiefen-Schreiben");
        check(memcmp(before, c.mem + DEPTH, sizeof(before)) == 0,
              "D24-Gastpuffer bleibt ohne Tiefen-Schreiben unveraendert");
    }


    /* (E2) CPU-Schreibzugriff der Gast-CPU zwischen zwei Kommandolisten:
       der zweite HW-Draw muss das Ziel aus dem Gast neu laden, sonst
       ueberschreibt der Flush den CPU-Wert mit dem alten D3D-Stand. */
    {
        uint32_t size, offl, offr;
        offl = morton_off(2, 7, 4u);
        offr = morton_off(12, 7, 4u);
        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        size = scene_half(&c, 0);
        gpu_run_pica_list(&c, LIST, size);
        memset(c.mem + FB, 0x5A, 16 * 16 * 4);
        size = scene_half(&c, 1);
        gpu_run_pica_list(&c, LIST, size);
        check(c.mem[FB + offl] == 0x5A && c.mem[FB + offl + 1] == 0x5A &&
              c.mem[FB + offl + 2] == 0x5A && c.mem[FB + offl + 3] == 0x5A,
              "CPU-Schreibzugriff zwischen zwei Listen bleibt erhalten");
        check(c.mem[FB + offr + 3] == 0x10,
              "zweite Liste zeichnet in das neu geladene Ziel");
    }

    /* (E5) Textur aendert sich zwischen zwei Kommandolisten: der zweite Draw
       muss die neuen Gastbytes sampeln. Der Byte-Hash laeuft einmal pro Liste
       je Textur, ein Warmlauf-Cache darf den Wechsel nicht verschlucken. */
    {
        static unsigned char hwb[16 * 16 * 4], swb[16 * 16 * 4];
        uint32_t sz;
        int bad, one, edge, first;

        /* Hardware: Liste 1 mit Seed 1, dann Gasttextur auf Seed 2 aendern. */
        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        sz = scene_f(&c);
        gpu_run_pica_list(&c, LIST, sz);
        fill_tex_rgba8(&c, TEX0, 2);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        sz = scene_tex_cmd(&c);
        gpu_run_pica_list(&c, LIST, sz);
        memcpy(hwb, c.mem + FB, sizeof(hwb));

        /* Software-Referenz mit genau diesen Gastbytes. */
        gpu_pica_reset();
        gpu_pica_set_render_hw(0);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        fill_tex_rgba8(&c, TEX0, 2);
        sz = scene_tex_cmd(&c);
        gpu_run_pica_list(&c, LIST, sz);
        memcpy(swb, c.mem + FB, sizeof(swb));

        bad = compare_pixels(swb, hwb, (int)sizeof(swb), 4, &one, &edge, &first);
        if (bad > 0)
            printf("     Texturwechsel: %d Pixel >1 LSB (erster %d), %d Kantenpixel\n",
                   bad, first, edge);
        check(bad <= 32, "Texturinhalt zwischen zwei Listen wird neu gesampelt");
    }

    /* (E3) D24 ueber D32_FLOAT: Laden und Rueckschreiben ohne Draw muss
       bitgleich sein (keine 1-LSB-Drift durch den Float-Umweg). */
    {
        HwDrawState st;
        static unsigned char d0[16 * 16 * 4], d1[16 * 16 * 4];
        int x, y;
        for (y = 0; y < 16; y++) {
            int yp = 15 - y;
            for (x = 0; x < 16; x++) {
                uint32_t off = morton_off(x, yp, 3u) +
                               (uint32_t)(yp & ~7) * 16u * 3u;
                uint32_t z = ((uint32_t)(x * 16 + y) * 2654435761u) & 0xFFFFFFu;
                unsigned char *p = c.mem + ((DEPTH + off) & 0x1FFFFFFFu);
                if (x == 0 && y == 0) z = 0xFFFFFFu;
                if (x == 1 && y == 0) z = 0x000001u;
                p[0] = (unsigned char)(z & 0xFFu);
                p[1] = (unsigned char)((z >> 8) & 0xFFu);
                p[2] = (unsigned char)((z >> 16) & 0xFFu);
            }
        }
        memcpy(d0, c.mem + DEPTH, 16 * 16 * 3);
        hw_test_state(&st, &c, FB, 0xFF0000FFu);
        st.depth_mem = c.mem + DEPTH;
        st.depth_size = (uint32_t)c.mem_size - DEPTH;
        st.depth_format = 2u;              /* D24 */
        st.depth_test = 0;
        st.depth_write = 1;
        check(hw_begin_draw(&st) != 0, "D24-Ziel fuer Bitgenauigkeit");
        hw_triangles(g_hw_off, 3);         /* setzt depth_dirty, kein Pixel */
        hw_flush_to_guest();
        memcpy(d1, c.mem + DEPTH, 16 * 16 * 3);
        check(memcmp(d0, d1, 16 * 16 * 3) == 0,
              "D24 laden+zurueckschreiben bitgleich (ohne Draw)");
    }

    /* (E1) Render-to-Texture: der erste Draw nach einem Zielwechsel muss die
       soeben gerenderte Textur neu hochladen. hw_begin_draw liest die
       Texturgeneration deshalb erst NACH dem Flush des alten Ziels; ohne den
       Fix samplet dieser Draw den alten Cache-Stand (hier: Null statt Farbe). */
    {
        HwDrawState st;
        static const HwVertex quad[6] = {
            {{-1.0f, -1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0.0f, 0.0f}, {0, 0}, {0, 0}}},
            {{ 1.0f, -1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{1.0f, 0.0f}, {0, 0}, {0, 0}}},
            {{ 1.0f,  1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{1.0f, 1.0f}, {0, 0}, {0, 0}}},
            {{-1.0f, -1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0.0f, 0.0f}, {0, 0}, {0, 0}}},
            {{ 1.0f,  1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{1.0f, 1.0f}, {0, 0}, {0, 0}}},
            {{-1.0f,  1.0f, 0.0f, 1.0f}, {1, 1, 1, 1}, {{0.0f, 1.0f}, {0, 0}, {0, 0}}},
        };
        HwDrawState stc, stb;
        int i;

        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        memset(c.mem + RTTDST, 0, 16 * 16 * 4);

        hw_test_state(&st, &c, FB, 0u);
        st.tex[0].enabled = 1;
        st.tex[0].offset = FB;
        st.tex[0].bytes = 16u * 16u * 4u;
        st.tex[0].format = 0u;
        st.tex[0].width = 16u;
        st.tex[0].height = 16u;
        st.tex[0].wrap_s = 2u;
        st.tex[0].wrap_t = 2u;
        st.tex[0].type = 0u;
        st.tev_src[0] = 0x00030003u;         /* Replace(Texture0) */
        st.tev_ops[0] = 0u;
        check(hw_begin_draw(&st) != 0, "RTT: Texturziel gebunden");
        hw_triangles(quad, 6);               /* Cache-Slot mit Nullinhalt */

        stc = st;
        stc.tev_src[0] = 0x000E000Eu;        /* Replace(Constant) */
        stc.tev_cst[0] = 0x80402010u;
        check(hw_begin_draw(&stc) != 0, "RTT: konstante Farbe in das Ziel");
        hw_triangles(quad, 6);

        stb = st;
        stb.color_mem = c.mem + RTTDST;
        stb.color_size = (uint32_t)c.mem_size - RTTDST;
        check(hw_begin_draw(&stb) != 0, "RTT: Zielwechsel nach dem Rendern");
        hw_triangles(quad, 6);
        hw_flush_to_guest();
        i = (int)morton_off(8, 7, 4u);
        {
            const unsigned char *px = c.mem + RTTDST + i;
            printf("     RTT-Pixel: %02X%02X%02X%02X (erwartet 80402010)\n",
                   px[0], px[1], px[2], px[3]);
            check(px[0] == 0x80u && px[1] == 0x40u && px[2] == 0x20u &&
                  px[3] == 0x10u,
                  "Render-to-Texture samplet das gerade gerenderte Ziel");
        }
    }

    /* (E4) Abwechselnd Hardware- und Software-Draws in einer Liste: die
       Zahl der vollen Transfers je Wechsel wird gemessen (Bericht). */
    {
        unsigned long long d0, f0, d1, f1, k0, r0, k1, r1, u0, u1, l0, l1;
        clock_t t0, t1;
        uint32_t size;
        gpu_pica_reset();
        gpu_pica_set_render_hw(1);
        memset(c.mem + FB, 0, 16 * 16 * 4);
        size = scene_u(&c);
        hw_stats(&d0, &f0);
        hw_target_sync_stats(&k0, &r0);
        u0 = hw_texture_uploads();
        l0 = hw_target_loads();
        t0 = clock();
        gpu_run_pica_list(&c, LIST, size);
        t1 = clock();
        hw_stats(&d1, &f1);
        hw_target_sync_stats(&k1, &r1);
        u1 = hw_texture_uploads();
        l1 = hw_target_loads();
        printf("     HW/SW-Wechsel: Draws=%llu Flushes=%llu Ladungen=%llu Reloads=%llu "
               "gehalten=%llu Uploads=%llu, %.2f ms\n",
               d1 - d0, f1 - f0, l1 - l0, r1 - r0, k1 - k0, u1 - u0,
               1000.0 * (double)(t1 - t0) / (double)CLOCKS_PER_SEC);
        check(d1 - d0 >= 2u, "zwei Hardware-Draws im HW/SW-Wechsel");
        check(f1 - f0 >= 2u, "je SW-Draw mit ausstehendem HW-Stand ein Readback");
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
