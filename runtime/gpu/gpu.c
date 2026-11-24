/* PICA200-Kommandolisten-Ausfuehrer + Software-Rasterizer.
 *
 * gpu_run_pica_list() liest Paare (value, header) wie die echte GPU
 * (docs/REFERENCE.md, PICA200) und fuehrt Draws (0x22E/0x22F) aus.
 *
 * Vertex-Pipeline: Vertex-Shader-Interpreter (0x2B0..0x2DF: Uniforms,
 * Programm, Swizzle, Arithmetik, CALL/IF/LOOP/BREAK) und Attribute-Loader
 * (0x200..0x227 inkl. Defaults, Index-Puffer 0x22F, Immediate 0x232) nach
 * Azahar shader_interpreter.cpp/vertex_loader.cpp; Ausgangs-Mapping
 * 0x04F..0x056 (POSITION, COLOR, TEXCOORD0..2).
 * Fragment-Pipeline: Sutherland-Hodgman-Clipping, 12.4-Coverage, TEV
 * 0x0C0..0x0FD, Texturen 0x080..0x09F (alle 14 Formate, 8x8-Morton,
 * Wrap/Border/Filter; nur Einheit 0 beachtet den Typ, TEXCOORD2 = 22/23),
 * Output-Merger 0x100..0x107 (Blending, LogicOp, Alpha-Test) und
 * Farb-/Tiefenziel 0x110..0x11E (Kanalmasken, RGBA8/RGB8/
 * RGB5A1/RGB565/RGBA4, Depth-Test/Write, Stencil, D16/D24/D24S8) sowie
 * Fog (0xE0/0xE1/0xE6/0xE8..0xEF, Azahar sw_rasterizer.cpp WriteFog).
 * Noch offen (ROADMAP M5): Lighting, Mipmaps, Proctex.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "arm_cpu.h"

#define GPU_FB_DEFAULT 0x1F000000u  /* Port-Fenster liest hier */

static uint32_t g_fb = GPU_FB_DEFAULT;
static uint32_t g_clear = 0x000000u;
static int g_fb_w = 400;
static int g_fb_h = 240;

/* Determinismus-Zaehler: abgeschlossene Listen/Submits und Draws. */
static volatile unsigned long long g_pica_lists = 0;
static volatile unsigned long long g_pica_draws = 0;

unsigned long long gpu_pica_list_count(void) { return g_pica_lists; }
unsigned long long gpu_pica_draw_count(void) { return g_pica_draws; }

void gpu_set_fb(uint32_t addr) { if (addr != 0) g_fb = addr; }
uint32_t gpu_get_fb(void) { return g_fb; }
void gpu_set_fb_size(uint32_t addr, int w, int h) {
    if (addr != 0) g_fb = addr;
    if (w > 0) g_fb_w = w;
    if (h > 0) g_fb_h = h;
}
int gpu_get_fb_width(void) { return g_fb_w; }
int gpu_get_fb_height(void) { return g_fb_h; }

static void put_px(ArmCpu *c, int x, int y, uint32_t rgb)
{
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return;
    uint32_t off = (g_fb & 0x1FFFFFFFu) + (uint32_t)(y * g_fb_w + x) * 3u;
    if (off + 3u > c->mem_size) return;
    arm_mem_write(c, off + 0u, 1, (rgb >> 0) & 0xFFu);
    arm_mem_write(c, off + 1u, 1, (rgb >> 8) & 0xFFu);
    arm_mem_write(c, off + 2u, 1, (rgb >> 16) & 0xFFu);
}



static void put_px_blend(ArmCpu *c, int x, int y, uint32_t rgb, uint32_t a)
{
    uint32_t off;
    if (x < 0 || y < 0 || x >= g_fb_w || y >= g_fb_h) return;
    if (a == 0u) return;
    off = (g_fb & 0x1FFFFFFFu) + (uint32_t)(y * g_fb_w + x) * 3u;
    if (off + 3u > c->mem_size) return;
    if (a >= 255u) {
        put_px(c, x, y, rgb & 0xFFFFFFu);
        return;
    }
    {
        uint32_t ia = 255u - (a > 255u ? 255u : a);
        uint32_t dr = c->mem[off + 0u], dg = c->mem[off + 1u], db = c->mem[off + 2u];
        uint32_t sr = rgb & 0xFFu, sg = (rgb >> 8) & 0xFFu, sb = (rgb >> 16) & 0xFFu;
        c->mem[off + 0u] = (uint8_t)((sr * a + dr * ia) / 255u);
        c->mem[off + 1u] = (uint8_t)((sg * a + dg * ia) / 255u);
        c->mem[off + 2u] = (uint8_t)((sb * a + db * ia) / 255u);
    }
}



/* ================= echte PICA200-Kommandoliste =================
 * Format (Azahar pica_core.cpp, ProcessCmdList/WriteInternalReg): Strom aus
 * Paaren
 *     value, header
 * header: bits 0..15 cmd_id (RegsInternal-Index), bits 16..19 parameter_mask,
 *         bits 20..27 extra_data_length, bit 31 group_commands.
 * Jeder Record beginnt 8-Byte-ausgerichtet; nach einem Record mit ungerader
 * Wortzahl folgt ein Pad-Wort. Bei extra_data_length > 0 folgen weitere
 * value-Woerter: group=1 schreibt cmd_id+1..cmd_id+n (sequentielle Register),
 * group=0 schreibt cmd_id n-mal (Programm-/LUT-Upload).
 * Trigger: 0x22E/0x22F Draw, 0x010 Finalize (P3D-IRQ, irq_autostop),
 *          0x23C/0x23D startet Kanal 0/1 des Kommandopuffers (0x238-0x23D).
 * Schreiben wird byteweise entsprechend parameter_mask angewandt
 * (Azahar ExpandBitsToBytes).
 */
#define PICA_NREGS 0x300

/* Physische Adressen der PICA (Azahar memory.h): VRAM liegt physisch bei
 * 0x18000000 und im Gast-Adressraum bei 0x1F000000; FCRAM physisch bei
 * 0x20000000, im Port lineare Fortsetzung des Linear-Heaps ab 0x14000000. */
#define PICA_VRAM_PADDR   0x18000000u
#define PICA_VRAM_VADDR   0x1F000000u
#define PICA_VRAM_SIZE    0x00600000u
#define PICA_FCRAM_PADDR  0x20000000u
#define PICA_LINEAR_VADDR 0x14000000u
#define PICA_LINEAR_SIZE  0x08000000u

/* Von GSP gesetzt; 0x010 Finalize meldet P3D (Azahar
 * PicaCore::SetInterruptHandler / HandleSpecialReg(irq_request)). */
static void (*p_irq_handler)(int id) = NULL;

/* Defaults wie Azahar PicaCore::InitializeRegs (von GSP gesetzt). */
static uint32_t p_regs[PICA_NREGS] = {
    [0x010] = 0x00000000u,  /* irq_request */
    [0x020] = 0x12345678u,  /* irq_compare */
    [0x030] = 0xFFFFFFF0u,  /* irq_mask */
    [0x034] = 0x00000001u,  /* irq_autostop */
};

/* Fog-LUT (0xE8..0xEF schreiben bei 0xE6 % 128, danach Offset++). */
static uint32_t g_fog_lut[128];

static void p_vs_reset(void);
static void p_vs_reset_draw_state(void);

uint32_t gpu_pica_phys_to_guest(uint32_t paddr)
{
    if (paddr >= PICA_VRAM_PADDR && paddr < PICA_VRAM_PADDR + PICA_VRAM_SIZE)
        return PICA_VRAM_VADDR + (paddr - PICA_VRAM_PADDR);
    if (paddr >= PICA_FCRAM_PADDR && paddr < PICA_FCRAM_PADDR + PICA_LINEAR_SIZE)
        return PICA_LINEAR_VADDR + (paddr - PICA_FCRAM_PADDR);
    return paddr;
}

uint32_t gpu_pica_virt_to_phys(uint32_t vaddr)
{
    if (vaddr >= PICA_VRAM_VADDR && vaddr < PICA_VRAM_VADDR + PICA_VRAM_SIZE)
        return PICA_VRAM_PADDR + (vaddr - PICA_VRAM_VADDR);
    if (vaddr >= PICA_LINEAR_VADDR && vaddr < PICA_LINEAR_VADDR + PICA_LINEAR_SIZE)
        return PICA_FCRAM_PADDR + (vaddr - PICA_LINEAR_VADDR);
    return vaddr;
}

void gpu_pica_set_irq_handler(void (*fn)(int id))
{
    p_irq_handler = fn;
}

uint32_t gpu_pica_reg(uint32_t id)
{
    return (id < PICA_NREGS) ? p_regs[id] : 0u;
}

void gpu_pica_reset(void)
{
    memset(p_regs, 0, sizeof(p_regs));
    memset(g_fog_lut, 0, sizeof(g_fog_lut));
    p_regs[0x020] = 0x12345678u;
    p_regs[0x030] = 0xFFFFFFF0u;
    p_regs[0x034] = 0x00000001u;
    p_vs_reset();
    p_vs_reset_draw_state();
}

void gpu_pica_bind_texture(uint32_t addr, int w, int h, int fmt)
{
    if (w <= 0 || h <= 0) return;
    p_regs[0x80] |= 1u;                       /* main_config.texture0_enable */
    p_regs[0x81] = 0u;                        /* border_color */
    p_regs[0x82] = (((uint32_t)w & 0x7FFu) << 16) |
                   ((uint32_t)h & 0x7FFu);    /* width / height */
    p_regs[0x83] = 0u;                        /* clamp, 2D */
    p_regs[0x85] = (addr >> 3) & 0x0FFFFFFFu; /* address / 8 */
    p_regs[0x8E] = (uint32_t)fmt & 0xFu;      /* 1 = RGB8 */
}

int gpu_pica_texture_bound(void)
{
    return (p_regs[0x80] & 1u) && (p_regs[0x82] & 0x07FF07FFu) != 0u;
}

/* UV-Rechteck fuer das Vollbild-Quad (z. B. 400x240-Unterbereich eines
   512x256-Sprite-Atlas). Default 0..1. */
static float p_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};
void gpu_pica_set_uv(float u0, float v0, float u1, float v1)
{
    p_uv[0] = u0; p_uv[1] = v0; p_uv[2] = u1; p_uv[3] = v1;
}
void gpu_pica_get_uv(float *u0, float *v0, float *u1, float *v1)
{
    if (u0) *u0 = p_uv[0];
    if (v0) *v0 = p_uv[1];
    if (u1) *u1 = p_uv[2];
    if (v1) *v1 = p_uv[3];
}

static uint32_t p_read32(ArmCpu *c, uint32_t off)
{
    uint32_t v = 0;
    off &= 0x1FFFFFFFu;
    if (off + 4u <= c->mem_size) memcpy(&v, c->mem + off, 4);
    return v;
}

/* ===================== Vertex-Shader-Zustand =====================
 * Azahar ShaderSetup/ShaderRegs: Registerbereich 0x2B0..0x2DF.
 *   0x2B0 bool_uniforms (1 Bit je Uniform)
 *   0x2B1..0x2B4 int_uniforms[4] (4 Byte je Uniform)
 *   0x2C0 uniform_setup.index (+ Bit31 = Float32-Format),
 *   0x2C1..0x2C8 set_value (gepackte float24/float32-Queue)
 *   0x2CB program.offset, 0x2CC..0x2D3 set_word
 *   0x2D5 swizzle_patterns.offset, 0x2D6..0x2DD set_word
 * Default-Attribute/Immediate: 0x232 index, 0x233..0x235 set_value.
 */
#define VS_PROG_WORDS 512u
#define VS_SWZ_WORDS  4096u
#define VS_MAX_STEPS  0x10000u

static float g_vs_def_attr[16][4];   /* input_default_attributes (0x232) */
static float g_vs_float[96][4];      /* uniforms.f */
static unsigned char g_vs_bool[16];
static unsigned char g_vs_int[4][4];
static uint32_t g_vs_code[VS_PROG_WORDS];
static unsigned g_vs_code_words;
static uint32_t g_vs_swz[VS_SWZ_WORDS];
static unsigned g_vs_pack_n;         /* Uniform-Packing-Queue */
static uint32_t g_vs_pack[4];

static void p_vs_reset(void)
{
    memset(g_vs_def_attr, 0, sizeof(g_vs_def_attr));
    memset(g_vs_float, 0, sizeof(g_vs_float));
    memset(g_vs_bool, 0, sizeof(g_vs_bool));
    memset(g_vs_int, 0, sizeof(g_vs_int));
    memset(g_vs_code, 0, sizeof(g_vs_code));
    memset(g_vs_swz, 0, sizeof(g_vs_swz));
    g_vs_code_words = 0;
    g_vs_pack_n = 0;
}

/* Azahar Pica::Float<16,7>::FromRaw: normal/denormal/inf-nan. */
static float p_f24_to_float(uint32_t hex)
{
    int32_t exponent = (int32_t)((hex >> 16) & 0x7Fu);
    uint32_t mantissa = hex & 0xFFFFu;
    uint32_t fp32;
    uint32_t sign = (hex >> 23) << 31;
    if (exponent == 0 && mantissa == 0) {
        fp32 = sign;
    } else if (exponent == 0) {
        exponent = 65;              /* bias + 1, bias = 127 - 63 */
        while ((mantissa & (1u << 16)) == 0) {
            exponent--;
            mantissa <<= 1;
        }
        mantissa &= 0xFFFFu;
        fp32 = sign | ((uint32_t)exponent << 23) | (mantissa << 7);
    } else if (exponent == 0x7F) {
        fp32 = sign | (0xFFu << 23) | (mantissa << 7);
    } else {
        fp32 = sign | ((uint32_t)(64 + exponent) << 23) | (mantissa << 7);
    }
    {
        float f;
        memcpy(&f, &fp32, 4);
        return f;
    }
}

static float p_attr_comp(ArmCpu *c, uint32_t addr, int fmt)
{
    uint32_t o = addr & 0x1FFFFFFFu;
    if (fmt == 3) { float f = 0.0f; if (o + 4u <= c->mem_size) memcpy(&f, c->mem + o, 4); return f; }
    if  (fmt == 2) { int16_t s = 0; if (o + 2u <= c->mem_size) memcpy(&s, c->mem + o, 2); return (float)s; }
    if (fmt == 1) { uint8_t u = 0; if (o + 1u <= c->mem_size) u = c->mem[o]; return (float)u; }
    { int8_t b = 0; if (o + 1u <= c->mem_size) b = (int8_t)c->mem[o]; return (float)b; }
}

/* ===================== Vertex-Attribut-Loader =====================
 * Semantik wie Azahar pica/vertex_loader.cpp + regs_pipeline.h:
 *  - 0x200 base_address (Bits 1..28), physisch: base = value * 16
 *  - 0x201/0x202: 12 Format-Nibbles (Bits 0..1 Format, 2..3 Komponenten-1)
 *  - 0x202 Bits 16..27 attribute_mask: gesetzt = Default-Attribut (kein Load)
 *  - 0x202 Bits 28..31 max_attribute_index (Gesamtzahl Attribute - 1)
 *  - 0x203 + 3*l: data_offset, Komponentennibbles, byte_count/Stride,
 *    component_count. Innerhalb eines Loaders wird der Offset vor jeder
 *    Komponente auf ihre Elementgroesse ausgerichtet (AlignUp), Attribute
 *    12..15 sind 4/8/12/16-Byte-Padding.
 * Default-Attribute kommen aus 0x232/0x233..0x235 (Immediate-Queue).
 */
typedef struct {
    uint32_t src[16];
    uint32_t stride[16];
    int fmt[16];
    int ncomp[16];
    int isdef[16];
    uint32_t base;
    int total;
} PicaAttrCfg;

static int p_attr_elem_size(int fmt)
{
    return (fmt == 3) ? 4 : (fmt == 2) ? 2 : 1;
}

static void p_attr_config(PicaAttrCfg *ac)
{
    uint32_t i, l;
    ac->base = gpu_pica_phys_to_guest(((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u);
    ac->total = (int)((p_regs[0x202] >> 28) & 0xFu) + 1;
    for (i = 0; i < 16; i++) {
        uint32_t nib;
        ac->src[i] = 0; ac->stride[i] = 0;
        ac->isdef[i] = (i >= 12) || ((p_regs[0x202] & (1u << (16u + i))) != 0u);
        if (i < 8) nib = (p_regs[0x201] >> (i * 4)) & 0xFu;
        else nib = (p_regs[0x202] >> ((i - 8) * 4)) & 0xFu;
        ac->fmt[i] = (int)(nib & 3u);
        ac->ncomp[i] = (int)((nib >> 2) & 3u) + 1;
    }
    for (l = 0; l < 12; l++) {
        uint32_t w0 = p_regs[0x203 + l * 3];
        uint32_t w1 = p_regs[0x204 + l * 3];
        uint32_t w2 = p_regs[0x205 + l * 3];
        uint32_t off = 0;
        uint32_t data_offset = w0 & 0x0FFFFFFFu;
        uint32_t stride = (w2 >> 16) & 0xFFu;
        uint32_t cc = (w2 >> 28) & 0xFu;
        uint32_t k;
        for (k = 0; k < cc && k < 12; k++) {
            uint32_t ai = (k < 8) ? ((w1 >> (k * 4)) & 0xFu)
                                  : ((w2 >> ((k - 8) * 4)) & 0xFu);
            if (ai < 12) {
                int es = p_attr_elem_size(ac->fmt[ai]);
                off = (off + (uint32_t)es - 1u) & ~((uint32_t)es - 1u);
                ac->src[ai] = data_offset + off;
                ac->stride[ai] = stride;
                off += (uint32_t)(ac->ncomp[ai] * es);
            } else if (ai < 16) {
                off = (off + 3u) & ~3u;
                off += (ai - 11u) * 4u;
            }
        }
    }
}

/* Liest die Attribute von Vertex `vert` (ohne Index-Umrechnung) nach
   out[attr][0..3]. Nicht geladene/default Attribute kommen aus vs.def_attr. */
static void p_load_attrs(ArmCpu *c, uint32_t vert, float out[16][4])
{
    PicaAttrCfg ac;
    int i, k;
    p_attr_config(&ac);
    for (i = 0; i < 16; i++) {
        for (k = 0; k < 4; k++) out[i][k] = 0.0f;
        if (i >= ac.total) continue;
        if (ac.isdef[i]) {
            for (k = 0; k < 4; k++) out[i][k] = g_vs_def_attr[i][k];
            continue;
        }
        {
            uint32_t a = ac.base + ac.src[i] + ac.stride[i] * vert;
            int es = p_attr_elem_size(ac.fmt[i]);
            for (k = 0; k < ac.ncomp[i] && k < 4; k++)
                out[i][k] = p_attr_comp(c, a + (uint32_t)(k * es), ac.fmt[i]);
        }
        for (k = ac.ncomp[i]; k < 4; k++) out[i][k] = (k == 3) ? 1.0f : 0.0f;
    }
}

/* ================= Texturen (0x080..0x09F) =================
 * Azahar texture_decode.cpp: Jede 8x8-Kachel liegt Morton-verschachtelt
 * (VideoCore::MortonInterleave) im Speicher; Texturen liegen von unten nach
 * oben, deshalb wird t vor dem Sample invertiert. Formate
 * (TexturingRegs::TextureFormat): 0 RGBA8, 1 RGB8, 2 RGB5A1, 3 RGB565,
 * 4 RGBA4, 5 IA8, 6 RG8 (HILO8), 7 I8, 8 A8, 9 IA4, 10 I4, 11 A4,
 * 12 ETC1, 13 ETC1A4.
 * Kanalreihenfolge wie Azahar common/color.h: RGBA8 im Speicher a,b,g,r
 * (32-Bit-Wort 0xRRGGBBAA), RGB8 b,g,r, IA8 a,i. */
static uint32_t p_tex_tile_size(uint32_t fmt)
{
    switch (fmt) {
    case 0: return 4u * 64u;                 /* RGBA8 */
    case 1: return 3u * 64u;                 /* RGB8 */
    case 2: case 3: case 4: case 5: case 6: return 2u * 64u;
    case 7: case 8: case 9: return 64u;
    case 10: case 11: return 32u;            /* I4/A4, 4 Bit */
    case 12: return 32u;                     /* ETC1: 4 Subtiles * 8 B */
    case 13: return 64u;                     /* ETC1A4: 4 Subtiles * 16 B */
    default: return 0u;
    }
}

static uint32_t p_morton_interleave(uint32_t x, uint32_t y)
{
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    return xlut[x % 8u] + ylut[y % 8u];
}

static unsigned p_c4to8(unsigned v) { v &= 0xFFu; return (unsigned)(uint8_t)((v << 4) | v); }
static unsigned p_c5to8(unsigned v) { v &= 0xFFu; return (unsigned)(uint8_t)((v << 3) | (v >> 2)); }
static unsigned p_c6to8(unsigned v) { v &= 0xFFu; return (unsigned)(uint8_t)((v << 2) | (v >> 4)); }

/* ETC1-Subtile (Azahar texture/etc1.cpp SampleETC1Subtile). */
static void p_etc1_texel(uint64_t t, unsigned x, unsigned y, unsigned char out[3])
{
    static const int mod[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42},
                                  {18, 60}, {24, 80}, {33, 106}, {47, 183}};
    unsigned texel = 4u * x + y;
    unsigned idx1 = (unsigned)((t >> 37) & 7u);
    unsigned idx2 = (unsigned)((t >> 34) & 7u);
    int r, g, b, i;

    if ((t >> 32) & 1u) {
        unsigned s = x;
        x = y;
        y = s;
    }
    if ((t >> 33) & 1u) {
        r = (int)((t >> 59) & 0x1Fu);
        g = (int)((t >> 51) & 0x1Fu);
        b = (int)((t >> 43) & 0x1Fu);
        if (x >= 2) {
            int dr = (int)((t >> 56) & 7u);
            int dg = (int)((t >> 48) & 7u);
            int db = (int)((t >> 40) & 7u);
            if (dr & 4) dr -= 8;
            if (dg & 4) dg -= 8;
            if (db & 4) db -= 8;
            r += dr;
            g += dg;
            b += db;
        }
        r = (int)p_c5to8((unsigned)r);
        g = (int)p_c5to8((unsigned)g);
        b = (int)p_c5to8((unsigned)b);
    } else {
        unsigned sb = (x < 2) ? 44u : 40u;
        unsigned sg = (x < 2) ? 52u : 48u;
        unsigned sr = (x < 2) ? 60u : 56u;
        b = (int)p_c4to8((unsigned)((t >> sb) & 0xFu));
        g = (int)p_c4to8((unsigned)((t >> sg) & 0xFu));
        r = (int)p_c4to8((unsigned)((t >> sr) & 0xFu));
    }
    i = mod[(x < 2) ? idx1 : idx2][(unsigned)((t >> texel) & 1u)];
    if ((t >> (16u + texel)) & 1u) i = -i;
    r += i;
    g += i;
    b += i;
    out[0] = (unsigned char)(r < 0 ? 0 : (r > 255 ? 255 : r));
    out[1] = (unsigned char)(g < 0 ? 0 : (g > 255 ? 255 : g));
    out[2] = (unsigned char)(b < 0 ? 0 : (b > 255 ? 255 : b));
}

/* Ein Texel einer 8x8-Kachel dekodieren; mo = p_morton_interleave(fx, fy). */
static void p_decode_texel(const uint8_t *tile, uint32_t fmt, uint32_t mo,
                           unsigned char out[4])
{
    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    switch (fmt) {
    case 0: {                                          /* RGBA8: a,b,g,r */
        const uint8_t *s = tile + mo * 4u;
        out[0] = s[3];
        out[1] = s[2];
        out[2] = s[1];
        out[3] = s[0];
        break;
    }
    case 1: {                                          /* RGB8: b,g,r */
        const uint8_t *s = tile + mo * 3u;
        out[0] = s[2];
        out[1] = s[1];
        out[2] = s[0];
        break;
    }
    case 2: case 3: case 4: {                          /* 16 Bit */
        const uint8_t *s = tile + mo * 2u;
        unsigned v = (unsigned)s[0] | ((unsigned)s[1] << 8);
        if (fmt == 2u) {                               /* RGB5A1 */
            out[0] = (unsigned char)p_c5to8((v >> 11) & 0x1Fu);
            out[1] = (unsigned char)p_c5to8((v >> 6) & 0x1Fu);
            out[2] = (unsigned char)p_c5to8((v >> 1) & 0x1Fu);
            out[3] = (unsigned char)((v & 1u) ? 255u : 0u);
        } else if (fmt == 3u) {                        /* RGB565 */
            out[0] = (unsigned char)p_c5to8((v >> 11) & 0x1Fu);
            out[1] = (unsigned char)p_c6to8((v >> 5) & 0x3Fu);
            out[2] = (unsigned char)p_c5to8(v & 0x1Fu);
        } else {                                       /* RGBA4 */
            out[0] = (unsigned char)p_c4to8((v >> 12) & 0xFu);
            out[1] = (unsigned char)p_c4to8((v >> 8) & 0xFu);
            out[2] = (unsigned char)p_c4to8((v >> 4) & 0xFu);
            out[3] = (unsigned char)p_c4to8(v & 0xFu);
        }
        break;
    }
    case 5: {                                          /* IA8: a,i */
        const uint8_t *s = tile + mo * 2u;
        out[0] = out[1] = out[2] = s[1];
        out[3] = s[0];
        break;
    }
    case 6: {                                          /* RG8 (HILO8) */
        const uint8_t *s = tile + mo * 2u;
        out[0] = s[1];
        out[1] = s[0];
        out[2] = 0;
        break;
    }
    case 7: {                                          /* I8 */
        out[0] = out[1] = out[2] = tile[mo];
        break;
    }
    case 8: {                                          /* A8 */
        out[0] = out[1] = out[2] = 0;
        out[3] = tile[mo];
        break;
    }
    case 9: {                                          /* IA4: iiii aaaa */
        unsigned v = tile[mo];
        out[0] = out[1] = out[2] = (unsigned char)p_c4to8((v >> 4) & 0xFu);
        out[3] = (unsigned char)p_c4to8(v & 0xFu);
        break;
    }
    case 10: case 11: {                                /* I4 / A4 */
        const uint8_t *s = tile + mo / 2u;
        unsigned v = (mo & 1u) ? ((*s & 0xF0u) >> 4) : (*s & 0x0Fu);
        v = p_c4to8(v);
        if (fmt == 10u) {
            out[0] = out[1] = out[2] = (unsigned char)v;
        } else {
            out[0] = out[1] = out[2] = 0;
            out[3] = (unsigned char)v;
        }
        break;
    }
    case 12: case 13: {                                /* ETC1 / ETC1A4 */
        unsigned has_alpha = (fmt == 13u);
        unsigned ssz = has_alpha ? 16u : 8u;
        unsigned fx = mo & 7u;
        unsigned fy = mo >> 3;
        const uint8_t *s = tile + (fx / 4u + 2u * (fy / 4u)) * ssz;
        unsigned char rgb[3];
        if (has_alpha) {
            uint64_t pa = 0;
            unsigned k;
            for (k = 0; k < 8u; k++) pa |= (uint64_t)s[k] << (8u * k);
            out[3] = (unsigned char)p_c4to8((unsigned)((pa >> (4u * (fx * 4u + fy))) & 0xFu));
            s += 8;
        }
        {
            uint64_t td = 0;
            unsigned k;
            for (k = 0; k < 8u; k++) td |= (uint64_t)s[k] << (8u * k);
            p_etc1_texel(td, fx % 4u, fy % 4u, rgb);
        }
        out[0] = rgb[0];
        out[1] = rgb[1];
        out[2] = rgb[2];
        break;
    }
    default:
        break;
    }
}

/* Register-Gruppen der Textureinheiten: 0x81/0x91/0x99, Format
   0x8E/0x96/0x9E (TexturingRegs, ASSERT_REG_POSITION). */
static uint32_t p_tex_base(uint32_t unit)
{
    return (unit == 0u) ? 0x81u : ((unit == 1u) ? 0x91u : 0x99u);
}

static uint32_t p_tex_fmt_reg(uint32_t unit)
{
    return (unit == 0u) ? 0x8Eu : ((unit == 1u) ? 0x96u : 0x9Eu);
}

/* Wrap-Modus (Azahar sw_texturing.cpp GetWrappedTexCoord). */
static int p_wrap_coord(uint32_t mode, int val, int size)
{
    switch (mode) {
    case 4u:                                   /* ClampToEdge2 */
        if (val < 0) return (int)((unsigned)val % (unsigned)size);
        /* fallthrough */
    case 0u:                                   /* ClampToEdge */
        if (val < 0) val = 0;
        if (val > size - 1) val = size - 1;
        return val;
    case 1u:                                   /* ClampToBorder */
        return val;
    case 2u: case 5u: case 6u: case 7u:        /* Repeat/Border2/Repeat2/3 */
        return (int)((unsigned)val % (unsigned)size);
    case 3u: {                                 /* MirroredRepeat */
        unsigned c = (unsigned)val % (unsigned)(2 * size);
        if (c >= (unsigned)size) c = (unsigned)(2 * size) - 1u - c;
        return (int)c;
    }
    default:
        return (int)((unsigned)val % (unsigned)size);
    }
}

/* Texel einer Einheit an (x,y) ohne Wrap (Koordinaten bereits im Bild). */
static int p_lookup_texel(ArmCpu *c, uint32_t unit, int x, int y,
                          unsigned char out[4])
{
    uint32_t size = p_regs[p_tex_base(unit) + 1u];
    uint32_t fmt = p_regs[p_tex_fmt_reg(unit)] & 0xFu;
    int w = (int)((size >> 16) & 0x7FFu);
    int h = (int)(size & 0x7FFu);
    uint32_t ts = p_tex_tile_size(fmt);
    uint32_t off;

    if (w <= 0 || h <= 0 || ts == 0u) return 0;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    off = gpu_pica_phys_to_guest((p_regs[p_tex_base(unit) + 4u] & 0x0FFFFFFFu) * 8u)
        + (uint32_t)(y / 8) * ts * (uint32_t)(w / 8)
        + (uint32_t)(x / 8) * ts;
    if (off + ts > c->mem_size) return 0;
    p_decode_texel(c->mem + off, fmt,
                   p_morton_interleave((uint32_t)(x & 7), (uint32_t)(y & 7)), out);
    return 1;
}

/* Eine Adresse inkl. Wrap/Border/Farbtiefe (t invertiert) sampeln. */
static int p_texel_wrapped(ArmCpu *c, uint32_t unit, int s, int t,
                           unsigned char out[4])
{
    uint32_t base = p_tex_base(unit);
    uint32_t cfg = p_regs[base + 2u];
    uint32_t size = p_regs[base + 1u];
    uint32_t border = p_regs[base];
    uint32_t ws = (cfg >> 12) & 7u;
    uint32_t wt = (cfg >> 8) & 7u;
    int w = (int)((size >> 16) & 0x7FFu);
    int h = (int)(size & 0x7FFu);
    int border_s, border_t;

    out[0] = out[1] = out[2] = 0;
    out[3] = 255;
    if (w <= 0 || h <= 0) return 0;
    border_s = (ws == 1u) ? (s < 0 || s >= w) : (ws == 5u && s >= w);
    border_t = (wt == 1u) ? (t < 0 || t >= h) : (wt == 5u && t >= h);
    if (border_s || border_t) {
        out[0] = (unsigned char)(border & 0xFFu);
        out[1] = (unsigned char)((border >> 8) & 0xFFu);
        out[2] = (unsigned char)((border >> 16) & 0xFFu);
        out[3] = (unsigned char)((border >> 24) & 0xFFu);
        return 1;
    }
    s = p_wrap_coord(ws, s, w);
    t = p_wrap_coord(wt, t, h);
    if (s < 0 || s >= w || t < 0 || t >= h) {
        out[3] = 0;
        return 0;
    }
    return p_lookup_texel(c, unit, s, h - 1 - t, out);
}

/* Sample einer Textureinheit 0..2 (Azahar sw_rasterizer.cpp TextureColor).
 * Filter: Hardware waehlt mag/min ueber die LOD; ohne Ableitungen wird
 * linear verwendet, sobald einer der beiden Filter linear ist. */
static void p_sample_tex(ArmCpu *c, uint32_t unit, float u, float v,
                         unsigned char out[4])
{
    uint32_t base = p_tex_base(unit);
    uint32_t cfg = p_regs[base + 2u];
    uint32_t size = p_regs[base + 1u];
    int w = (int)((size >> 16) & 0x7FFu);
    int h = (int)(size & 0x7FFu);
    int linear;

    out[0] = out[1] = out[2] = out[3] = 0;
    if (!(p_regs[0x80] & (1u << unit))) return;   /* Azahar: {0,0,0,0} */
    if (unit == 0u && ((cfg >> 28) & 7u) == 5u) return;   /* Azahar: nur Einheit 0 beachtet den Typ */
    if ((p_regs[base + 4u] & 0x0FFFFFFFu) == 0u) { out[3] = 255; return; }
    if (w <= 0 || h <= 0) return;

    linear = (int)(((cfg >> 1) & 1u) | ((cfg >> 2) & 1u));
    if (!linear) {
        if (!p_texel_wrapped(c, unit, (int)(u * (float)w), (int)(v * (float)h), out))
            out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    {
        float fs = u * (float)w - 0.5f;
        float ft = v * (float)h - 0.5f;
        int s0 = (int)floorf(fs);
        int t0 = (int)floorf(ft);
        float a = fs - (float)s0;
        float b = ft - (float)t0;
        unsigned char p00[4], p10[4], p01[4], p11[4];
        int i;
        p_texel_wrapped(c, unit, s0, t0, p00);
        p_texel_wrapped(c, unit, s0 + 1, t0, p10);
        p_texel_wrapped(c, unit, s0, t0 + 1, p01);
        p_texel_wrapped(c, unit, s0 + 1, t0 + 1, p11);
        for (i = 0; i < 4; i++) {
            float top = (float)p00[i] * (1.0f - a) + (float)p10[i] * a;
            float bot = (float)p01[i] * (1.0f - a) + (float)p11[i] * a;
            float val = top * (1.0f - b) + bot * b;
            out[i] = (unsigned char)(val < 0.0f ? 0.0f : (val > 255.0f ? 255.0f : roundf(val)));
        }
    }
}

/* ================= TEV (Fragment-Combiner) =================
 * Nachbildung von Azahar renderer_software/sw_rasterizer.cpp WriteTevConfig
 * und sw_texturing.cpp (GetColorModifier/GetAlphaModifier/ColorCombine/
 * AlphaCombine). Register 0x0C0..0x0FD, 6 Stufen a 8 Woerter:
 *   +0 sources, +1 modifiers, +2 ops, +3 const_color, +4 scales.
 */
typedef struct { unsigned char c[4]; } PicaColor;
typedef struct { float pos[4]; float color[4]; float uv[3][2]; } PicaVertex;

/* ================= Fog (Azahar sw_rasterizer.cpp WriteFog) =================
 * 0xE0 Bits0..2 fog_mode (0 None, 5 Fog, 7 Gas), Bit16 fog_flip; 0xE1
 * fog_color r/g/b. Die 128 LUT-Eintraege werden ueber 0xE8..0xEF geladen:
 * jedes geschriebene Wort landet bei fog_lut_offset (0xE6) % 128, danach
 * inkrementiert die Hardware den Offset (pica_core.cpp HandleSpecialReg).
 * Eintrag: Bits13..23 value (0.0.11), Bits0..12 difference (1.1.11).
 */

static void p_fog_lut_write(uint32_t value)
{
    uint32_t off = p_regs[0xE6] & 0xFFFFu;
    g_fog_lut[off % 128u] = value;
    p_regs[0xE6] = (off + 1u) & 0xFFFFu;   /* Azahar: fog_lut_offset++ */
}

/* Ergebnis von TEV in-place: out = factor*out + (1-factor)*fog_color.
 * Reihenfolge im Azahar-Pixelshader: Alpha-Test, Fog, Depth/Stencil. */
static void p_apply_fog(float depth, PicaColor *out)
{
    uint32_t mode = p_regs[0xE0] & 7u;
    uint32_t color = p_regs[0xE1];
    float fog_index, fog_i_f, fog_f, fog_factor;
    float value, diff;
    uint32_t entry;
    unsigned i;

    if (mode != 5u) return;                /* nur FogMode::Fog */
    fog_index = ((p_regs[0xE0] & (1u << 16)) ? (1.0f - depth) : depth) * 128.0f;
    /* Azahar clampt nur den LUT-Index, nicht die Fraktion. */
    fog_i_f = floorf(fog_index);
    if (fog_i_f < 0.0f) fog_i_f = 0.0f;
    if (fog_i_f > 127.0f) fog_i_f = 127.0f;
    fog_f = fog_index - fog_i_f;
    entry = g_fog_lut[(unsigned)fog_i_f];
    value = (float)((entry >> 13) & 0x7FFu) / 2047.0f;
    diff = (float)(((int32_t)(entry << 19)) >> 19) / 2047.0f;
    fog_factor = value + diff * fog_f;
    if (fog_factor < 0.0f) fog_factor = 0.0f;
    if (fog_factor > 1.0f) fog_factor = 1.0f;
    for (i = 0; i < 3u; i++) {
        float fog_c = (float)((color >> (8u * i)) & 0xFFu);
        out->c[i] = (unsigned char)(fog_factor * (float)out->c[i] +
                                    (1.0f - fog_factor) * fog_c);
    }
}


static const unsigned TEV_STAGE_BASE[6] = {0xC0u, 0xC8u, 0xD0u, 0xD8u, 0xF0u, 0xF8u};

static PicaColor p_tev_source(unsigned src, const PicaColor tex[4],
                              const PicaColor *prim, const PicaColor *pfc,
                              const PicaColor *sfc, const PicaColor *prevbuf,
                              uint32_t const_color, const PicaColor *prev)
{
    PicaColor r;
    int i;
    switch (src) {
    case 0x0: return *prim;
    case 0x1: return *pfc;
    case 0x2: return *sfc;
    case 0x3: return tex[0];
    case 0x4: return tex[1];
    case 0x5: return tex[2];
    case 0x6: return tex[3];
    case 0xD: return *prevbuf;
    case 0xE:
        r.c[0] = (unsigned char)(const_color & 0xFFu);
        r.c[1] = (unsigned char)((const_color >> 8) & 0xFFu);
        r.c[2] = (unsigned char)((const_color >> 16) & 0xFFu);
        r.c[3] = (unsigned char)((const_color >> 24) & 0xFFu);
        return r;
    case 0xF: return *prev;
    default:
        for (i = 0; i < 4; i++) r.c[i] = 0;
        return r;
    }
}

/* ColorModifier liefert einen RGB-Vektor (Komponenten 0..2). */
static void p_color_modifier(unsigned m, const PicaColor *s, unsigned char out[3])
{
    unsigned char v = 0;
    switch (m) {
    case 0x0: out[0] = s->c[0]; out[1] = s->c[1]; out[2] = s->c[2]; return;
    case 0x1:
        out[0] = (unsigned char)(255 - s->c[0]);
        out[1] = (unsigned char)(255 - s->c[1]);
        out[2] = (unsigned char)(255 - s->c[2]);
        return;
    case 0x2: v = s->c[3]; out[0] = out[1] = out[2] = v; return;
    case 0x3: v = s->c[3]; out[0] = out[1] = out[2] = (unsigned char)(255 - v); return;
    case 0x4: v = s->c[0]; out[0] = out[1] = out[2] = v; return;
    case 0x5: v = s->c[0]; out[0] = out[1] = out[2] = (unsigned char)(255 - v); return;
    case 0x8: v = s->c[1]; out[0] = out[1] = out[2] = v; return;
    case 0x9: v = s->c[1]; out[0] = out[1] = out[2] = (unsigned char)(255 - v); return;
    case 0xC: v = s->c[2]; out[0] = out[1] = out[2] = v; return;
    case 0xD: v = s->c[2]; out[0] = out[1] = out[2] = (unsigned char)(255 - v); return;
    default: out[0] = out[1] = out[2] = 0; return;
    }
}

static unsigned char p_alpha_modifier(unsigned m, const PicaColor *s)
{
    switch (m) {
    case 0: return s->c[3];
    case 1: return (unsigned char)(255 - s->c[3]);
    case 2: return s->c[0];
    case 3: return (unsigned char)(255 - s->c[0]);
    case 4: return s->c[1];
    case 5: return (unsigned char)(255 - s->c[1]);
    case 6: return s->c[2];
    case 7: return (unsigned char)(255 - s->c[2]);
    default: return 0;
    }
}

static void p_color_combine(unsigned op, const unsigned char in[3][3],
                            unsigned char out[3])
{
    int i;
    switch (op) {
    case 0: /* Replace */
        for (i = 0; i < 3; i++) out[i] = in[0][i];
        break;
    case 1: /* Modulate */
        for (i = 0; i < 3; i++) out[i] = (unsigned char)(in[0][i] * in[1][i] / 255);
        break;
    case 2: /* Add */
        for (i = 0; i < 3; i++) { int v = in[0][i] + in[1][i]; out[i] = (unsigned char)(v > 255 ? 255 : v); }
        break;
    case 3: /* AddSigned */
        for (i = 0; i < 3; i++) {
            int v = (int)in[0][i] + in[1][i] - 128;
            if (v < 0) v = 0; if (v > 255) v = 255;
            out[i] = (unsigned char)v;
        }
        break;
    case 4: /* Lerp */
        for (i = 0; i < 3; i++)
            out[i] = (unsigned char)((in[0][i] * in[2][i] + in[1][i] * (255 - in[2][i])) / 255);
        break;
    case 5: /* Subtract */
        for (i = 0; i < 3; i++) { int v = (int)in[0][i] - in[1][i]; out[i] = (unsigned char)(v < 0 ? 0 : v); }
        break;
    case 6: case 7: { /* Dot3_RGB / Dot3_RGBA */
        int res = ((in[0][0] * 2 - 255) * (in[1][0] * 2 - 255) + 128) / 256 +
                  ((in[0][1] * 2 - 255) * (in[1][1] * 2 - 255) + 128) / 256 +
                  ((in[0][2] * 2 - 255) * (in[1][2] * 2 - 255) + 128) / 256;
        if (res < 0) res = 0; if (res > 255) res = 255;
        out[0] = out[1] = out[2] = (unsigned char)res;
        break;
    }
    case 8: /* MultiplyThenAdd */
        for (i = 0; i < 3; i++) {
            int v = (in[0][i] * in[1][i] + 255 * in[2][i]) / 255;
            out[i] = (unsigned char)(v > 255 ? 255 : v);
        }
        break;
    case 9: /* AddThenMultiply */
        for (i = 0; i < 3; i++) {
            int v = in[0][i] + in[1][i];
            if (v > 255) v = 255;
            out[i] = (unsigned char)(v * in[2][i] / 255);
        }
        break;
    default:
        out[0] = out[1] = out[2] = 0;
        break;
    }
}

static unsigned char p_alpha_combine(unsigned op, const unsigned char in[3])
{
    int v;
    switch (op) {
    case 0: return in[0];
    case 1: return (unsigned char)(in[0] * in[1] / 255);
    case 2: v = in[0] + in[1]; return (unsigned char)(v > 255 ? 255 : v);
    case 3: v = (int)in[0] + in[1] - 128; if (v < 0) v = 0; if (v > 255) v = 255; return (unsigned char)v;
    case 4: return (unsigned char)((in[0] * in[2] + in[1] * (255 - in[2])) / 255);
    case 5: v = (int)in[0] - in[1]; return (unsigned char)(v < 0 ? 0 : v);
    case 8: v = (in[0] * in[1] + 255 * in[2]) / 255; return (unsigned char)(v > 255 ? 255 : v);
    case 9: v = in[0] + in[1]; if (v > 255) v = 255; return (unsigned char)(v * in[2] / 255);
    default: return 0;     /* Alpha hat kein Dot3 */
    }
}

static PicaColor p_tev(const PicaColor tex[4], const PicaColor *primary)
{
    PicaColor zero = {{0, 0, 0, 0}};
    PicaColor output = {{0, 0, 0, 0}};
    PicaColor buffer = {{0, 0, 0, 0}};
    PicaColor next;
    uint32_t buf_in = p_regs[0xE0];
    uint32_t buf_col = p_regs[0xFD];
    unsigned s;
    int i;

    next.c[0] = (unsigned char)(buf_col & 0xFFu);
    next.c[1] = (unsigned char)((buf_col >> 8) & 0xFFu);
    next.c[2] = (unsigned char)((buf_col >> 16) & 0xFFu);
    next.c[3] = (unsigned char)((buf_col >> 24) & 0xFFu);

    for (s = 0; s < 6; s++) {
        uint32_t base = TEV_STAGE_BASE[s];
        uint32_t src = p_regs[base + 0];
        uint32_t mod = p_regs[base + 1];
        uint32_t ops = p_regs[base + 2];
        uint32_t cst = p_regs[base + 3];
        uint32_t scl = p_regs[base + 4];
        unsigned cs1 = src & 0xFu, cs2 = (src >> 4) & 0xFu, cs3 = (src >> 8) & 0xFu;
        unsigned as1 = (src >> 16) & 0xFu, as2 = (src >> 20) & 0xFu, as3 = (src >> 24) & 0xFu;
        unsigned cm1 = mod & 0xFu, cm2 = (mod >> 4) & 0xFu, cm3 = (mod >> 8) & 0xFu;
        unsigned am1 = (mod >> 12) & 7u, am2 = (mod >> 16) & 7u, am3 = (mod >> 20) & 7u;
        unsigned cop = ops & 0xFu, aop = (ops >> 16) & 0xFu;
        unsigned cmul = ((scl & 3u) < 3u) ? (1u << (scl & 3u)) : 1u;
        unsigned amul = (((scl >> 16) & 3u) < 3u) ? (1u << ((scl >> 16) & 3u)) : 1u;
        PicaColor src1, src2, src3, asrc1, asrc2, asrc3;
        unsigned char cin[3][3];
        unsigned char ain[3];
        unsigned char cout[3];
        unsigned char aout;

        if (s == 0) {
            if (cs1 == 0xFu) cs1 = cs3;
            if (cs2 == 0xFu) cs2 = cs3;
        }
        src1 = p_tev_source(cs1, tex, primary, &zero, &zero, &buffer, cst, &output);
        src2 = p_tev_source(cs2, tex, primary, &zero, &zero, &buffer, cst, &output);
        src3 = p_tev_source(cs3, tex, primary, &zero, &zero, &buffer, cst, &output);
        p_color_modifier(cm1, &src1, cin[0]);
        p_color_modifier(cm2, &src2, cin[1]);
        p_color_modifier(cm3, &src3, cin[2]);
        p_color_combine(cop, cin, cout);

        if (cop == 7) {         /* Dot3_RGBA: Alpha = Rot */
            aout = cout[0];
        } else {
            asrc1 = p_tev_source(as1, tex, primary, &zero, &zero, &buffer, cst, &output);
            asrc2 = p_tev_source(as2, tex, primary, &zero, &zero, &buffer, cst, &output);
            asrc3 = p_tev_source(as3, tex, primary, &zero, &zero, &buffer, cst, &output);
            ain[0] = p_alpha_modifier(am1, &asrc1);
            ain[1] = p_alpha_modifier(am2, &asrc2);
            ain[2] = p_alpha_modifier(am3, &asrc3);
            aout = p_alpha_combine(aop, ain);
        }

        for (i = 0; i < 3; i++) {
            unsigned v = (unsigned)cout[i] * cmul;
            output.c[i] = (unsigned char)(v > 255u ? 255u : v);
        }
        {
            unsigned v = (unsigned)aout * amul;
            output.c[3] = (unsigned char)(v > 255u ? 255u : v);
        }

        buffer = next;
        if (s < 4 && (buf_in & (1u << (8u + s)))) {
            next.c[0] = output.c[0]; next.c[1] = output.c[1]; next.c[2] = output.c[2];
        }
        if (s < 4 && (buf_in & (1u << (12u + s)))) next.c[3] = output.c[3];
    }
    return output;
}

/* ================= Farbziel (Framebuffer 0x110..0x11E) =================
 * Azahar sw_framebuffer.cpp: Adresse 0x11C * 8 physisch, Breite 0x11D
 * Bits 0..10, Hoehe (Feld) Bits 12..21 = tatsaechliche Hoehe - 1,
 * Kachelung 8x8 Morton, Zeilen von unten nach oben. */
static int p_fb_configured(void)
{
    return (p_regs[0x11C] & 0x0FFFFFFFu) != 0u;
}

static int p_fb_bytes_per_px(void)
{
    switch ((p_regs[0x116] >> 16) & 7u) {
    case 1: return 3;                 /* RGB8 */
    case 2: case 3: case 4: return 2; /* RGB5A1 / RGB565 / RGBA4 */
    default: return 4;                /* RGBA8 */
    }
}

static uint32_t p_morton_offset(uint32_t x, uint32_t y, uint32_t bpp)
{
    static const uint32_t xlut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
    static const uint32_t ylut[8] = {0x00, 0x02, 0x08, 0x0A, 0x20, 0x22, 0x28, 0x2A};
    uint32_t i = xlut[x % 8u] + ylut[y % 8u];
    uint32_t coarse_x = x & ~7u;
    return (i + coarse_x * 8u) * bpp;
}

static uint32_t p_fb_pixel_offset(uint32_t x, uint32_t y)
{
    uint32_t width = p_regs[0x11D] & 0x7FFu;
    uint32_t hraw = (p_regs[0x11D] >> 12) & 0x3FFu;
    uint32_t bpp = (uint32_t)p_fb_bytes_per_px();
    if (width == 0u) width = (uint32_t)g_fb_w;
    y = hraw - y;                      /* Azahar: framebuffer.height - y */
    return p_morton_offset(x, y, bpp) + (y & ~7u) * width * bpp;
}

static void p_fb_read(ArmCpu *c, uint32_t x, uint32_t y, unsigned char out[4])
{
    uint32_t addr = gpu_pica_phys_to_guest((p_regs[0x11C] & 0x0FFFFFFFu) * 8u);
    uint32_t off = addr + p_fb_pixel_offset(x, y);
    uint32_t fmt = (p_regs[0x116] >> 16) & 7u;
    out[0] = out[1] = out[2] = out[3] = 0;
    switch (fmt) {
    case 1: /* RGB8: b,g,r (Azahar DecodeRGB8) */
        if (off + 3u <= c->mem_size) {
            out[2] = c->mem[off + 0u]; out[1] = c->mem[off + 1u]; out[0] = c->mem[off + 2u];
            out[3] = 255;
        }
        break;
    case 2: /* RGB5A1: r Bits11..15, g 6..10, b 1..5, a 0 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = c->mem[off] | ((uint32_t)c->mem[off + 1u] << 8);
            out[0] = (unsigned char)(((v >> 11) & 0x1Fu) * 255u / 31u);
            out[1] = (unsigned char)(((v >> 6) & 0x1Fu) * 255u / 31u);
            out[2] = (unsigned char)(((v >> 1) & 0x1Fu) * 255u / 31u);
            out[3] = (unsigned char)((v & 1u) ? 255u : 0u);
        }
        break;
    case 3: /* RGB565 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = c->mem[off] | ((uint32_t)c->mem[off + 1u] << 8);
            out[0] = (unsigned char)(((v >> 11) & 0x1Fu) * 255u / 31u);
            out[1] = (unsigned char)(((v >> 5) & 0x3Fu) * 255u / 63u);
            out[2] = (unsigned char)((v & 0x1Fu) * 255u / 31u);
            out[3] = 255;
        }
        break;
    case 4: /* RGBA4 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = c->mem[off] | ((uint32_t)c->mem[off + 1u] << 8);
            out[0] = (unsigned char)(((v >> 12) & 0xFu) * 17u);
            out[1] = (unsigned char)(((v >> 8) & 0xFu) * 17u);
            out[2] = (unsigned char)(((v >> 4) & 0xFu) * 17u);
            out[3] = (unsigned char)((v & 0xFu) * 17u);
        }
        break;
    default: /* RGBA8: a,b,g,r (Azahar DecodeRGBA8) */
        if (off + 4u <= c->mem_size) {
            out[3] = c->mem[off + 0u]; out[2] = c->mem[off + 1u];
            out[1] = c->mem[off + 2u]; out[0] = c->mem[off + 3u];
        }
        break;
    }
}

static void p_fb_write(ArmCpu *c, uint32_t x, uint32_t y, const unsigned char in[4])
{
    uint32_t addr = gpu_pica_phys_to_guest((p_regs[0x11C] & 0x0FFFFFFFu) * 8u);
    uint32_t off = addr + p_fb_pixel_offset(x, y);
    uint32_t fmt = (p_regs[0x116] >> 16) & 7u;
    unsigned r = in[0], g = in[1], b = in[2], a = in[3];
    switch (fmt) {
    case 1: /* RGB8: b,g,r */
        if (off + 3u <= c->mem_size) {
            c->mem[off] = (uint8_t)b; c->mem[off + 1] = (uint8_t)g; c->mem[off + 2] = (uint8_t)r;
        }
        break;
    case 2: /* RGB5A1 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a >> 7);
            c->mem[off] = (uint8_t)(v & 0xFFu); c->mem[off + 1] = (uint8_t)(v >> 8);
        }
        break;
    case 3: /* RGB565 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
            c->mem[off] = (uint8_t)(v & 0xFFu); c->mem[off + 1] = (uint8_t)(v >> 8);
        }
        break;
    case 4: /* RGBA4 */
        if (off + 2u <= c->mem_size) {
            uint32_t v = ((r >> 4) << 12) | ((g >> 4) << 8) | ((b >> 4) << 4) | (a >> 4);
            c->mem[off] = (uint8_t)(v & 0xFFu); c->mem[off + 1] = (uint8_t)(v >> 8);
        }
        break;
    default: /* RGBA8: a,b,g,r */
        if (off + 4u <= c->mem_size) {
            c->mem[off] = (uint8_t)a; c->mem[off + 1] = (uint8_t)b;
            c->mem[off + 2] = (uint8_t)g; c->mem[off + 3] = (uint8_t)r;
        }
        break;
    }
}

/* ================= Tiefe/Stencil (0x11B, 0x115/0x116) =================
 * Azahar sw_framebuffer.cpp GetDepth/SetDepth/GetStencil/SetStencil: gleiche
 * Kachelung wie das Farbziel, Bytes je Pixel nach Format (D16=2, D24=3,
 * D24S8=4), Werte little endian, Stencil im vierten Byte. */
static uint32_t p_depth_fmt(void) { return p_regs[0x116] & 3u; }

static uint32_t p_depth_bpp(void)
{
    switch (p_depth_fmt()) {
    case 0u: return 2u;               /* D16 */
    case 2u: return 3u;               /* D24 */
    default: return 4u;               /* D24S8 */
    }
}

static uint32_t p_depth_bits(void) { return (p_depth_fmt() == 0u) ? 16u : 24u; }

static uint32_t p_depth_pixel_offset(uint32_t x, uint32_t y)
{
    uint32_t width = p_regs[0x11D] & 0x7FFu;
    uint32_t hraw = (p_regs[0x11D] >> 12) & 0x3FFu;
    uint32_t bpp = p_depth_bpp();
    if (width == 0u) width = (uint32_t)g_fb_w;
    y = hraw - y;
    return p_morton_offset(x, y, bpp) + (y & ~7u) * width * bpp;
}

static int p_depth_addr(ArmCpu *c, uint32_t x, uint32_t y, uint32_t *out)
{
    uint32_t reg = p_regs[0x11B] & 0x0FFFFFFFu;
    uint32_t off;
    if (reg == 0u) return 0;
    off = gpu_pica_phys_to_guest(reg * 8u) + p_depth_pixel_offset(x, y);
    if (off + p_depth_bpp() > c->mem_size) return 0;
    *out = off;
    return 1;
}

static uint32_t p_depth_get(ArmCpu *c, uint32_t x, uint32_t y)
{
    uint32_t off = 0;
    if (!p_depth_addr(c, x, y, &off)) return 0xFFFFFFFFu;
    if (p_depth_fmt() == 0u)
        return (uint32_t)c->mem[off] | ((uint32_t)c->mem[off + 1u] << 8);
    return (uint32_t)c->mem[off] | ((uint32_t)c->mem[off + 1u] << 8)
         | ((uint32_t)c->mem[off + 2u] << 16);
}

static void p_depth_set(ArmCpu *c, uint32_t x, uint32_t y, uint32_t z)
{
    uint32_t off = 0;
    if ((p_regs[0x115] & 3u) == 0u) return;      /* allow_depth_stencil_write */
    if (!p_depth_addr(c, x, y, &off)) return;
    c->mem[off] = (uint8_t)(z & 0xFFu);
    if (p_depth_fmt() != 0u) {
        c->mem[off + 1u] = (uint8_t)((z >> 8) & 0xFFu);
        c->mem[off + 2u] = (uint8_t)((z >> 16) & 0xFFu);
    }
}

static uint32_t p_stencil_get(ArmCpu *c, uint32_t x, uint32_t y)
{
    uint32_t off = 0;
    if (p_depth_fmt() != 3u) return 0u;
    if (!p_depth_addr(c, x, y, &off)) return 0u;
    return c->mem[off + 3u];
}

static void p_stencil_set(ArmCpu *c, uint32_t x, uint32_t y, uint32_t s)
{
    uint32_t off = 0;
    if (p_depth_fmt() != 3u) return;
    if ((p_regs[0x115] & 3u) == 0u) return;
    if (!p_depth_addr(c, x, y, &off)) return;
    c->mem[off + 3u] = (uint8_t)(s & 0xFFu);
}

/* ---- Blending, LogicOp, Vergleiche (Azahar sw_framebuffer.cpp/sw_rasterizer.cpp) ---- */
static unsigned p_blend_factor(uint32_t f, int ch, const unsigned char src[4],
                               const unsigned char dst[4], uint32_t cst)
{
    unsigned cc = (cst >> (8 * ch)) & 0xFFu;
    unsigned ca = (cst >> 24) & 0xFFu;
    switch (f & 0xFu) {
    case 0u: return 0u;
    case 1u: return 255u;
    case 2u: return src[ch];
    case 3u: return 255u - src[ch];
    case 4u: return dst[ch];
    case 5u: return 255u - dst[ch];
    case 6u: return src[3];
    case 7u: return 255u - src[3];
    case 8u: return dst[3];
    case 9u: return 255u - dst[3];
    case 10u: return cc;
    case 11u: return 255u - cc;
    case 12u: return ca;
    case 13u: return 255u - ca;
    case 14u: {                                /* SourceAlphaSaturate */
        unsigned m = 255u - dst[3];
        return (ch == 3) ? 255u : (src[3] < m ? src[3] : m);
    }
    default: return src[ch];
    }
}

static int p_blend_eq(uint32_t eq, int s, int d)
{
    switch (eq & 7u) {
    case 1u: return (s - d) / 255;             /* Subtract */
    case 2u: return (d - s) / 255;             /* ReverseSubtract */
    case 3u: return (s < d ? s : d) / 255;     /* Min */
    case 4u: return (s > d ? s : d) / 255;     /* Max */
    default: return (s + d) / 255;             /* Add */
    }
}

static unsigned p_logic_op(uint32_t lop, unsigned s, unsigned d)
{
    switch (lop & 0xFu) {
    case 0u: return 0u;                        /* Clear */
    case 1u: return s & d;                     /* And */
    case 2u: return s & (~d & 0xFFu);          /* AndReverse */
    case 3u: return s;                         /* Copy */
    case 4u: return 255u;                      /* Set */
    case 5u: return (~s) & 0xFFu;              /* CopyInverted */
    case 6u: return d;                         /* NoOp */
    case 7u: return (~d) & 0xFFu;              /* Invert */
    case 8u: return (~(s & d)) & 0xFFu;        /* Nand */
    case 9u: return (s | d) & 0xFFu;           /* Or */
    case 10u: return (~(s | d)) & 0xFFu;       /* Nor */
    case 11u: return (s ^ d) & 0xFFu;          /* Xor */
    case 12u: return (~(s ^ d)) & 0xFFu;       /* Equiv */
    case 13u: return ((~s) & d) & 0xFFu;       /* AndInverted */
    case 14u: return (s | (~d)) & 0xFFu;       /* OrReverse */
    default: return ((~s) | d) & 0xFFu;        /* OrInverted */
    }
}

/* a <op> b; Reihenfolge wie Azahar (Stencil: ref,dest; Tiefe: z,ref_z). */
static int p_compare(uint32_t fn, unsigned a, unsigned b)
{
    switch (fn & 7u) {
    case 0u: return 0;                         /* Never */
    case 1u: return 1;                         /* Always */
    case 2u: return a == b;                    /* Equal */
    case 3u: return a != b;                    /* NotEqual */
    case 4u: return a < b;                     /* LessThan */
    case 5u: return a <= b;                    /* LessThanOrEqual */
    case 6u: return a > b;                     /* GreaterThan */
    default: return a >= b;                    /* GreaterThanOrEqual */
    }
}

static uint32_t p_stencil_action(uint32_t act, uint32_t old, uint32_t ref)
{
    switch (act & 7u) {
    case 1u: return 0u;                        /* Zero */
    case 2u: return ref & 0xFFu;               /* Replace */
    case 3u: return old < 254u ? old + 1u : 255u;   /* Increment (sat.) */
    case 4u: return old > 0u ? old - 1u : 0u;       /* Decrement (sat.) */
    case 5u: return (~old) & 0xFFu;            /* Invert */
    case 6u: return (old + 1u) & 0xFFu;        /* IncrementWrap */
    case 7u: return (old - 1u) & 0xFFu;        /* DecrementWrap */
    default: return old;                       /* Keep */
    }
}

/* Azahar sw_rasterizer.cpp DoDepthStencilTest. */
static int p_depth_stencil_test(ArmCpu *c, uint32_t x, uint32_t y, float depth)
{

    uint32_t dm = p_regs[0x107];
    uint32_t sf = p_regs[0x105];
    uint32_t so = p_regs[0x106];
    uint32_t stencil_enable = (sf & 1u) && (p_depth_fmt() == 3u);
    uint32_t old_stencil = 0u;
    uint32_t ref = (sf >> 16) & 0xFFu;

    if (stencil_enable) {
        uint32_t im = (sf >> 24) & 0xFFu;
        uint32_t wm = (sf >> 8) & 0xFFu;
        uint32_t ns;
        old_stencil = p_stencil_get(c, x, y);
        if (!p_compare((sf >> 4) & 7u, ref & im, old_stencil & im)) {
            ns = p_stencil_action(so, old_stencil, ref);
            p_stencil_set(c, x, y, (ns & wm) | (old_stencil & ~wm));
            return 0;
        }
    }
    if (dm & 1u) {                             /* depth_test_enable */
        uint32_t bits = p_depth_bits();
        uint32_t z = (uint32_t)(depth * (float)((1u << bits) - 1u));
        uint32_t ref_z = p_depth_get(c, x, y);
        if (!p_compare((dm >> 4) & 7u, z, ref_z)) {
            if (stencil_enable) {
                uint32_t wm = (sf >> 8) & 0xFFu;
                uint32_t ns = p_stencil_action((so >> 4) & 7u, old_stencil, ref);
                p_stencil_set(c, x, y, (ns & wm) | (old_stencil & ~wm));
            }
            return 0;
        }
    }
    if ((p_regs[0x115] & 3u) != 0u && (dm & (1u << 12))) {   /* depth_write_enable */
        p_depth_set(c, x, y, (uint32_t)(depth * (float)((1u << p_depth_bits()) - 1u)));
    }
    if (stencil_enable) {
        uint32_t wm = (sf >> 8) & 0xFFu;
        uint32_t ns = p_stencil_action((so >> 8) & 7u, old_stencil, ref);
        p_stencil_set(c, x, y, (ns & wm) | (old_stencil & ~wm));
    }
    return 1;
}


/* ================= Clipping (Azahar sw_rasterizer.cpp AddTriangle) ================= */
static float p_dot4(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

static void p_clip_lerp(float f, const PicaVertex *a, const PicaVertex *b, PicaVertex *o)
{
    int i, k;
    for (i = 0; i < 4; i++) {
        o->pos[i] = a->pos[i] * f + b->pos[i] * (1.0f - f);
        o->color[i] = a->color[i] * f + b->color[i] * (1.0f - f);
    }
    for (k = 0; k < 3; k++)
        for (i = 0; i < 2; i++)
            o->uv[k][i] = a->uv[k][i] * f + b->uv[k][i] * (1.0f - f);
}

/* Sutherland-Hodgman gegen eine Ebene; inside = dot(pos+bias, coeff) >= -eps. */
static int p_clip_edge(const PicaVertex *in, int n, PicaVertex *out,
                       const float coeff[4], const float bias[4])
{
    const float eps = 1e-8f;
    float bdot = p_dot4(bias, coeff);
    int i, m = 0;
    const PicaVertex *ref;

    if (n <= 0) return 0;
    ref = &in[n - 1];
    for (i = 0; i < n; i++) {
        const PicaVertex *v = &in[i];
        float dv = p_dot4(v->pos, coeff) + bdot;
        float dr = p_dot4(ref->pos, coeff) + bdot;
        int vin = dv >= -eps;
        int rin = dr >= -eps;
        if (vin) {
            if (!rin) {
                float f = dr / (dr - dv);
                p_clip_lerp(f, v, ref, &out[m++]);
            }
            out[m++] = *v;
        } else if (rin) {
            float f = dr / (dr - dv);
            p_clip_lerp(f, v, ref, &out[m++]);
        }
        ref = v;
    }
    return m;
}

static int p_signed_area(int x1, int y1, int x2, int y2, int x3, int y3)
{
    return (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
}

static int p_right_or_flat(int vx, int vy, int l1x, int l1y, int l2x, int l2y)
{
    if (l1y == l2y) return vy < l1y;
    return vx < l1x + (l2x - l1x) * (vy - l1y) / (l2y - l1y);
}

static void p_process_tri(ArmCpu *c, const PicaVertex *v0, const PicaVertex *v1,
                          const PicaVertex *v2, int reversed)
{
    int x[3], y[3];
    int area;
    uint32_t cull = p_regs[0x40] & 3u;

    x[0] = (int)roundf(v0->pos[0] * 16.0f);
    y[0] = (int)roundf(v0->pos[1] * 16.0f);
    x[1] = (int)roundf(v1->pos[0] * 16.0f);
    y[1] = (int)roundf(v1->pos[1] * 16.0f);
    x[2] = (int)roundf(v2->pos[0] * 16.0f);
    y[2] = (int)roundf(v2->pos[1] * 16.0f);
    area = p_signed_area(x[0], y[0], x[1], y[1], x[2], y[2]);

    if (cull == 0u || cull == 3u) {
        if (!reversed && area <= 0) { p_process_tri(c, v0, v2, v1, 1); return; }
    } else if (cull == 1u) {         /* KeepClockWise */
        if (!reversed) { p_process_tri(c, v0, v2, v1, 1); return; }
    } else {                         /* KeepCounterClockWise */
        if (area <= 0) return;
    }

    {
        int min_x = x[0] < x[1] ? (x[0] < x[2] ? x[0] : x[2]) : (x[1] < x[2] ? x[1] : x[2]);
        int min_y = y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2]) : (y[1] < y[2] ? y[1] : y[2]);
        int max_x = x[0] > x[1] ? (x[0] > x[2] ? x[0] : x[2]) : (x[1] > x[2] ? x[1] : x[2]);
        int max_y = y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2]) : (y[1] > y[2] ? y[1] : y[2]);
        uint32_t sc = p_regs[0x65];
        int sc_mode = (int)(sc & 3u);
        int sx1 = (int)(sc & 0x3FFu) << 4;
        int sy1 = (int)((sc >> 16) & 0x3FFu) << 4;
        int sx2 = (int)((p_regs[0x66] & 0x3FFu) + 1u) << 4;
        int sy2 = (int)(((p_regs[0x66] >> 16) & 0x3FFu) + 1u) << 4;
        int bias0, bias1, bias2;
        float invw[3];
        int py, px;

        if (sc_mode == 3) {              /* Include */
            if (min_x < sx1) min_x = sx1;
            if (min_y < sy1) min_y = sy1;
            if (max_x > sx2) max_x = sx2;
            if (max_y > sy2) max_y = sy2;
        }
        min_x &= ~0xF; min_y &= ~0xF;
        max_x = (max_x + 0xF) & ~0xF;
        max_y = (max_y + 0xF) & ~0xF;

        bias0 = p_right_or_flat(x[0], y[0], x[1], y[1], x[2], y[2]) ? -1 : 0;
        bias1 = p_right_or_flat(x[1], y[1], x[2], y[2], x[0], y[0]) ? -1 : 0;
        bias2 = p_right_or_flat(x[2], y[2], x[0], y[0], x[1], y[1]) ? -1 : 0;

        invw[0] = v0->pos[3];
        invw[1] = v1->pos[3];
        invw[2] = v2->pos[3];

        for (py = min_y + 8; py < max_y; py += 16) {
            for (px = min_x + 8; px < max_x; px += 16) {
                int w0, w1, w2, wsum;
                float bw0, bw1, bw2, iw, depth;
                float prim[4], tuv[3][2];
                PicaColor primary, tex_col[4];
                PicaColor tev_out;
                unsigned char texbuf[4];
                int i, unit;

                if (sc_mode == 1) {      /* Exclude */
                    if (px >= sx1 && px < sx2 && py >= sy1 && py < sy2) continue;
                }

                w0 = bias0 + p_signed_area(x[1], y[1], x[2], y[2], px, py);
                w1 = bias1 + p_signed_area(x[2], y[2], x[0], y[0], px, py);
                w2 = bias2 + p_signed_area(x[0], y[0], x[1], y[1], px, py);
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                wsum = w0 + w1 + w2;
                if (wsum == 0) continue;
                bw0 = (float)w0; bw1 = (float)w1; bw2 = (float)w2;
                iw = 1.0f / (invw[0] * bw0 + invw[1] * bw1 + invw[2] * bw2);

                for (i = 0; i < 4; i++)
                    prim[i] = (v0->color[i] * bw0 + v1->color[i] * bw1 + v2->color[i] * bw2) * iw;
                for (i = 0; i < 3; i++) {
                    tuv[i][0] = (v0->uv[i][0] * bw0 + v1->uv[i][0] * bw1 +
                                 v2->uv[i][0] * bw2) * iw;
                    tuv[i][1] = (v0->uv[i][1] * bw0 + v1->uv[i][1] * bw1 +
                                 v2->uv[i][1] * bw2) * iw;
                }

                for (i = 0; i < 4; i++) {
                    float f = prim[i];
                    if (f < 0.0f) f = 0.0f;
                    if (f > 1.0f) f = 1.0f;
                    primary.c[i] = (unsigned char)roundf(f * 255.0f);
                }
                for (i = 0; i < 4; i++)
                    tex_col[i].c[0] = tex_col[i].c[1] = tex_col[i].c[2] = tex_col[i].c[3] = 0;
                for (unit = 0; unit < 3u; unit++) {
                    int ci = (unit == 2u && ((p_regs[0x80] >> 13) & 1u)) ? 1 : (int)unit;
                    p_sample_tex(c, unit, tuv[ci][0], tuv[ci][1], texbuf);
                    for (i = 0; i < 4; i++) tex_col[unit].c[i] = texbuf[i];
                }

                tev_out = p_tev(tex_col, &primary);

                /* Alpha-Test (Azahar DoAlphaTest) */
                if (p_regs[0x104] & 1u) {
                    uint32_t fn = (p_regs[0x104] >> 4) & 7u;
                    uint32_t ref = (p_regs[0x104] >> 8) & 0xFFu;
                    if (!p_compare(fn, tev_out.c[3], ref)) continue;
                }

                /* Tiefe: Azahar screenpos.z interpoliert, dann Range/Offset
                 * (0x4D/0x4E) und optional W-Buffer (0x6D). */
                depth = (v0->pos[2] * bw0 + v1->pos[2] * bw1 + v2->pos[2] * bw2) /
                        (float)wsum;
                depth = depth * p_f24_to_float(p_regs[0x4D]) + p_f24_to_float(p_regs[0x4E]);
                if ((p_regs[0x6D] & 1u) == 0u)      /* DepthBuffering::WBuffering */
                    depth *= iw * (float)wsum;
                if (depth < 0.0f) depth = 0.0f;
                if (depth > 1.0f) depth = 1.0f;
                p_apply_fog(depth, &tev_out);
                if (p_fb_configured() && !p_depth_stencil_test(c, (uint32_t)(px >> 4),
                                                               (uint32_t)(py >> 4), depth))
                    continue;

                if (p_fb_configured()) {
                    unsigned char dest[4];
                    unsigned char result[4];
                    unsigned char bl[4];
                    uint32_t om = p_regs[0x100];
                    uint32_t mask = p_regs[0x107];
                    int k;
                    p_fb_read(c, (uint32_t)(px >> 4), (uint32_t)(py >> 4), dest);
                    if (om & (1u << 8)) {           /* alphablend_enable */
                        uint32_t p = p_regs[0x101];
                        uint32_t cst = p_regs[0x103];
                        for (k = 0; k < 4; k++) {
                            int sfac = (int)((k == 3) ? ((p >> 24) & 0xFu) : ((p >> 16) & 0xFu));
                            int dfac = (int)((k == 3) ? ((p >> 28) & 0xFu) : ((p >> 20) & 0xFu));
                            int sv = tev_out.c[k] * (int)p_blend_factor((uint32_t)sfac, k,
                                                                       tev_out.c, dest, cst);
                            int dv = dest[k] * (int)p_blend_factor((uint32_t)dfac, k,
                                                                   tev_out.c, dest, cst);
                            int r = p_blend_eq((k == 3) ? (p >> 8) : p, sv, dv);
                            if (r < 0) r = 0;
                            if (r > 255) r = 255;
                            bl[k] = (unsigned char)r;
                        }
                    } else {
                        uint32_t lop = p_regs[0x102] & 0xFu;
                        for (k = 0; k < 4; k++)
                            bl[k] = (unsigned char)p_logic_op(lop, tev_out.c[k], dest[k]);
                    }
                    result[0] = (mask & (1u << 8)) ? bl[0] : dest[0];
                    result[1] = (mask & (1u << 9)) ? bl[1] : dest[1];
                    result[2] = (mask & (1u << 10)) ? bl[2] : dest[2];
                    result[3] = (mask & (1u << 11)) ? bl[3] : dest[3];
                    if ((p_regs[0x113] & 0xFu) != 0u)
                        p_fb_write(c, (uint32_t)(px >> 4), (uint32_t)(py >> 4), result);
                } else {
                    put_px_blend(c, px >> 4, py >> 4,
                                 ((uint32_t)tev_out.c[0]) |
                                 (((uint32_t)tev_out.c[1]) << 8) |
                                 (((uint32_t)tev_out.c[2]) << 16),
                                 (uint32_t)tev_out.c[3]);
                }
            }
        }
    }
}

/* ---- Einzelner Vertex: Clip -> Viewport -> Rasterisierung ---- */
static void p_raster_tri(ArmCpu *c, const PicaVertex *a, const PicaVertex *b,
                         const PicaVertex *d)
{
    static const float coeff[7][4] = {
        {-1, 0, 0, 1}, {1, 0, 0, 1},
        {0, -1, 0, 1}, {0, 1, 0, 1},
        {0, 0, -1, 0}, {0, 0, 1, 1},
        {0, 0, 0, 1},
    };
    static const float bias[7][4] = {
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 1e-5f},
    };
    PicaVertex buf_a[16], buf_b[16];
    PicaVertex *in = buf_a, *out = buf_b;
    int n = 3, i;
    float half_x, half_y;
    int corner_x, corner_y;

    buf_a[0] = *a; buf_a[1] = *b; buf_a[2] = *d;

    for (i = 0; i < 7; i++) {
        int m = p_clip_edge(in, n, out, coeff[i], bias[i]);
        PicaVertex *t = in; in = out; out = t;
        n = m;
        if (n < 3) return;
    }
    if (p_regs[0x47] & 1u) {         /* clip_enable + clip_coef 0x48..0x4B */
        float cc[4];
        float zero[4] = {0, 0, 0, 0};
        cc[0] = p_f24_to_float(p_regs[0x48]);
        cc[1] = p_f24_to_float(p_regs[0x49]);
        cc[2] = p_f24_to_float(p_regs[0x4A]);
        cc[3] = p_f24_to_float(p_regs[0x4B]);
        n = p_clip_edge(in, n, out, cc, zero);
        { PicaVertex *t = in; in = out; out = t; }
        if (n < 3) return;
    }

    half_x = p_f24_to_float(p_regs[0x41]);
    half_y = p_f24_to_float(p_regs[0x43]);
    corner_x = (int)(p_regs[0x68] << 22) >> 22;   /* signed 10 Bit */
    corner_y = (int)(p_regs[0x68] << 6) >> 22;

    for (i = 0; i < n; i++) {
        float invw = 1.0f / in[i].pos[3];
        int k, m;
        in[i].pos[3] = invw;
        in[i].pos[2] *= invw;              /* Azahar screenpos.z = z / w */
        for (k = 0; k < 4; k++) in[i].color[k] *= invw;
        for (m = 0; m < 3; m++)
            for (k = 0; k < 2; k++) in[i].uv[m][k] *= invw;
        {
            float sx = (in[i].pos[0] * invw + 1.0f) * half_x + (float)corner_x;
            float sy = (in[i].pos[1] * invw + 1.0f) * half_y + (float)corner_y;
            in[i].pos[0] = sx;
            in[i].pos[1] = sy;
        }
    }

    for (i = 0; i < n - 2; i++) {
        p_process_tri(c, &in[0], &in[i + 1], &in[i + 2], 0);
    }
}

/* ================= Vertex-Shader-Interpreter =================
 * Nachbildung von Azahar shader/shader_interpreter.cpp (RunInterpreter).
 * Instruktionskodierung: nihstro shader_bytecode.h
 *   opcode = instr >> 26 (6 Bit)
 *   common: desc = bits 0..6, src2 7..11, src1 12..18
 *           (invertiert: src1i 14..18, src2i 7..13)
 *           addr_reg 19..20, dest 21..25
 *   flow:   num = 0..7, dest_offset = 10..21, op 22..23, refx/y 24/25
 */
typedef struct {
    float in[16][4];
    float tmp[16][4];
    float out[2][16][4];
    int bank;
    int cc[2];
    int32_t addr[3];
} VsUnit;

static float g_vs_ones[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static float g_vs_zeros[4] = {0.0f, 0.0f, 0.0f, 0.0f};
static float g_vs_discard[4];
static PicaVertex g_vs_last_vertex;
static int g_vs_have_last = 0;

static const float *p_vs_src(const VsUnit *u, uint32_t reg, int ari)
{
    if (ari) {
        int32_t off = u->addr[ari - 1];
        if (off < -128 || off > 127) off = 0;
        if (reg >= 0x20u && reg < 0x20u + 96u) {
            uint32_t idx = (uint32_t)((int32_t)(reg - 0x20u) + off) & 0x7Fu;
            if (idx >= 96u) return g_vs_ones;
            return g_vs_float[idx];
        }
    }
    if (reg < 0x10u) return u->in[reg];
    if (reg < 0x20u) return u->tmp[reg - 0x10u];
    if (reg < 0x20u + 96u) return g_vs_float[reg - 0x20u];
    return g_vs_zeros;
}

static float *p_vs_dest(VsUnit *u, uint32_t d)
{
    if (d < 0x10u) return u->out[u->bank][d];
    if (d < 0x20u) return u->tmp[d - 0x10u];
    return g_vs_discard;
}

static int p_vs_s1sel(uint32_t sw, int i) { return (int)((sw >> (11 - 2 * i)) & 3u); }
static int p_vs_s2sel(uint32_t sw, int i) { return (int)((sw >> (20 - 2 * i)) & 3u); }
static int p_vs_s3sel(uint32_t sw, int i) { return (int)((sw >> (29 - 2 * i)) & 3u); }
static float p_vs_mul(float a, float b)
{
    float r = a * b;
    if (r != r && a == a && b == b) r = 0.0f;   /* PICA: 0 statt NaN */
    return r;
}

static int p_vs_condition(const VsUnit *u, uint32_t instr)
{
    int rx = (int)((instr >> 25) & 1u);
    int ry = (int)((instr >> 24) & 1u);
    int op = (int)((instr >> 22) & 3u);
    int bx = (rx == u->cc[0]);
    int by = (ry == u->cc[1]);
    switch (op) {
    case 0: return bx || by;          /* Or */
    case 1: return bx && by;          /* And */
    case 2: return bx;                /* JustX */
    default: return by;                /* JustY */
    }
}

typedef struct { uint32_t els, end; } VsIf;
typedef struct { uint32_t end, ret; } VsCall;
typedef struct { uint32_t entry, end; unsigned down, inc, prev; } VsLoop;

static void p_vs_execute(VsUnit *u)
{
    VsIf ifs[8];
    VsCall calls[4];
    VsLoop loops[4];
    int ifn = 0, ifh = 0, cn = 0, ch = 0, ln = 0, lh = 0;
    uint32_t pc = 0;
    unsigned steps = 0;
    int stop = 0;

    while (!stop && steps++ < VS_MAX_STEPS) {
        uint32_t oldpc = pc;
        uint32_t instr, op;
        int is_break = 0;

        if (pc >= g_vs_code_words) break;
        instr = g_vs_code[pc];
        op = instr >> 26;

        if (op <= 0x1Fu || (op & ~1u) == 0x2Eu) {
            int inv = (op >= 0x18u && op <= 0x1Bu);
            uint32_t desc = instr & 0x7Fu;
            uint32_t s1r = inv ? ((instr >> 14) & 0x1Fu) : ((instr >> 12) & 0x7Fu);
            uint32_t s2r = inv ? ((instr >> 7) & 0x7Fu) : ((instr >> 7) & 0x1Fu);
            uint32_t ari = (instr >> 19) & 3u;
            uint32_t dreg = (instr >> 21) & 0x1Fu;
            uint32_t sw = g_vs_swz[desc & (VS_SWZ_WORDS - 1u)];
            const float *r1 = p_vs_src(u, s1r, inv ? 0 : (int)ari);
            const float *r2 = p_vs_src(u, s2r, inv ? (int)ari : 0);
            float s1[4], s2[4];
            float *dst;
            int i;

            for (i = 0; i < 4; i++) {
                s1[i] = r1[p_vs_s1sel(sw, i)];
                s2[i] = r2[p_vs_s2sel(sw, i)];
            }
            if (sw & (1u << 4)) for (i = 0; i < 4; i++) s1[i] = -s1[i];
            if (sw & (1u << 13)) for (i = 0; i < 4; i++) s2[i] = -s2[i];
            dst = p_vs_dest(u, dreg);

            if ((op & ~1u) == 0x2Eu) {
                int cx = (int)((instr >> 24) & 7u);
                int cy = (int)((instr >> 21) & 7u);
                for (i = 0; i < 2; i++) {
                    int cop = (i == 0) ? cx : cy;
                    int res = 0;
                    switch (cop) {
                    case 0: res = s1[i] == s2[i]; break;
                    case 1: res = s1[i] != s2[i]; break;
                    case 2: res = s1[i] < s2[i]; break;
                    case 3: res = s1[i] <= s2[i]; break;
                    case 4: res = s1[i] > s2[i]; break;
                    case 5: res = s1[i] >= s2[i]; break;
                    default: res = 0; break;
                    }
                    u->cc[i] = res;
                }
            }

            switch (op) {
            case 0x00u:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = s1[i] + s2[i];
                break;
            case 0x08u:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = p_vs_mul(s1[i], s2[i]);
                break;
            case 0x0Bu:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = floorf(s1[i]);
                break;
            case 0x0Cu:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = (s1[i] > s2[i]) ? s1[i] : s2[i];
                break;
            case 0x0Du:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = (s1[i] < s2[i]) ? s1[i] : s2[i];
                break;
            case 0x01u:
            case 0x02u:
            case 0x03u: {
                int nc = (op == 0x01u) ? 3 : 4;
                float dot = 0.0f;
                if (op == 0x03u) s1[3] = 1.0f;
                for (i = 0; i < nc; i++) dot += p_vs_mul(s1[i], s2[i]);
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = dot;
                break;
            }
            case 0x05u:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = exp2f(s1[0]);
                break;
            case 0x06u:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = log2f(s1[0]);
                break;
            case 0x0Eu:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = 1.0f / s1[0];
                break;
            case 0x0Fu:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = 1.0f / sqrtf(s1[0]);
                break;
            case 0x12u:
                for (i = 0; i < 2; i++)
                    if ((sw >> (3 - i)) & 1u) u->addr[i] = (int32_t)s1[i];
                break;
            case 0x13u:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = s1[i];
                break;
            case 0x09u:
            case 0x1Au:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = (s1[i] >= s2[i]) ? 1.0f : 0.0f;
                break;
            case 0x0Au:
            case 0x1Bu:
                for (i = 0; i < 4; i++)
                    if ((sw >> (3 - i)) & 1u) dst[i] = (s1[i] < s2[i]) ? 1.0f : 0.0f;
                break;
            default:
                break;      /* DST/LIT/unbekannt: Azahar schreibt nicht */
            }
        } else if (op >= 0x30u) {
            int inv = ((op & ~7u) == 0x30u);
            uint32_t desc = instr & 0x1Fu;
            uint32_t sw = g_vs_swz[desc & (VS_SWZ_WORDS - 1u)];
            uint32_t s1r = (instr >> 17) & 0x1Fu;
            uint32_t s2r, s3r;
            uint32_t ari = (instr >> 22) & 3u;
            uint32_t dreg = (instr >> 24) & 0x1Fu;
            const float *r1, *r2, *r3;
            float a[4], b[4], c[4];
            float *dst;
            int i;

            if (inv) {
                s3r = (instr >> 5) & 0x7Fu;
                s2r = (instr >> 12) & 0x1Fu;
            } else {
                s3r = (instr >> 5) & 0x1Fu;
                s2r = (instr >> 10) & 0x7Fu;
            }
            r1 = p_vs_src(u, s1r, 0);
            r2 = p_vs_src(u, s2r, inv ? 0 : (int)ari);
            r3 = p_vs_src(u, s3r, inv ? (int)ari : 0);
            for (i = 0; i < 4; i++) {
                a[i] = r1[p_vs_s1sel(sw, i)];
                b[i] = r2[p_vs_s2sel(sw, i)];
                c[i] = r3[p_vs_s3sel(sw, i)];
            }
            if (sw & (1u << 4)) for (i = 0; i < 4; i++) a[i] = -a[i];
            if (sw & (1u << 13)) for (i = 0; i < 4; i++) b[i] = -b[i];
            if (sw & (1u << 22)) for (i = 0; i < 4; i++) c[i] = -c[i];
            dst = p_vs_dest(u, dreg);
            for (i = 0; i < 4; i++)
                if ((sw >> (3 - i)) & 1u) dst[i] = p_vs_mul(a[i], b[i]) + c[i];
        } else {
            uint32_t num = instr & 0xFFu;
            uint32_t dest = (instr >> 10) & 0xFFFu;
            uint32_t bid = (instr >> 22) & 0xFu;
            int cond;

            switch (op) {
            case 0x22u:                /* END */
                stop = 1;
                break;
            case 0x2Cu:                /* JMPC */
                if (p_vs_condition(u, instr)) pc = dest - 1;
                break;
            case 0x2Du:                /* JMPU */
                cond = ((g_vs_bool[bid] != 0) == ((num & 1u) == 0u));
                if (cond) pc = dest - 1;
                break;
            case 0x24u:                /* CALL */
            case 0x25u:                /* CALLC */
            case 0x26u: {              /* CALLU */
                int take = 1;
                VsCall v;
                if (op == 0x26u) take = (g_vs_bool[bid] != 0);
                else if (op == 0x25u) take = p_vs_condition(u, instr);
                if (take) {
                    v.end = dest + num;
                    v.ret = pc + 1;
                    if (cn < 4) { calls[(ch + cn) & 3] = v; cn++; }
                    else { calls[ch] = v; ch = (ch + 1) & 3; }
                    pc = dest - 1;
                }
                break;
            }
            case 0x21u:                /* NOP */
                break;
            case 0x27u:                /* IFU */
            case 0x28u: {              /* IFC */
                VsIf v;
                cond = (op == 0x27u) ? (g_vs_bool[bid] != 0)
                                     : p_vs_condition(u, instr);
                if (cond) {
                    v.els = dest;
                    v.end = dest + num;
                    if (ifn < 8) { ifs[(ifh + ifn) & 7] = v; ifn++; }
                    else { ifs[ifh] = v; ifh = (ifh + 1) & 7; }
                } else {
                    pc = dest - 1;
                }
                break;
            }
            case 0x29u: {              /* LOOP */
                uint32_t iid = (instr >> 22) & 3u;
                VsLoop v;
                u->addr[2] = (int32_t)g_vs_int[iid][1];
                v.entry = pc + 1;
                v.end = dest + 1;
                v.down = g_vs_int[iid][0];
                v.inc = g_vs_int[iid][2];
                v.prev = (unsigned)g_vs_int[iid][1];
                if (ln < 4) { loops[(lh + ln) & 3] = v; ln++; }
                else { loops[lh] = v; lh = (lh + 1) & 3; }
                break;
            }
            case 0x23u:                /* BREAKC */
                if (p_vs_condition(u, instr)) is_break = 1;
                break;
            case 0x20u:                /* BREAK */
                is_break = 1;
                break;
            case 0x2Au:                /* EMIT (VS: ungueltig) */
            case 0x2Bu:                /* SETEMIT */
            default:
                break;
            }
        }

        pc++;
        {
            uint32_t next = oldpc + 1;
            unsigned k;
            for (k = 0; k < 4; k++) {
                VsCall *cb;
                if (cn == 0) break;
                cb = &calls[(ch + cn - 1) & 3];
                if (cb->end != next) break;
                if (k < 3) {
                    pc = cb->ret;
                    next = pc;
                }
                cn--;
            }
            if (ifn > 0) {
                VsIf *ib = &ifs[(ifh + ifn - 1) & 7];
                if (ib->els == oldpc + 1) {
                    pc = ib->end;
                    ifn--;
                }
            }
            if (ln > 0) {
                VsLoop *lb = &loops[(lh + ln - 1) & 3];
                if (lb->end == oldpc + 1 || is_break) {
                    u->addr[2] += (int32_t)lb->inc;
                    if (!is_break && lb->down > 0) {
                        lb->down--;
                        pc = lb->entry;
                    } else {
                        pc = lb->end;
                        if (ln > 1) u->addr[2] = (int32_t)lb->prev;
                        ln--;
                    }
                }
            }
        }
    }
}

/* ---- Ein-/Ausgangs-Mapping (Azahar shader_unit.cpp LoadInput/WriteOutput) ---- */
static void p_vs_process(const float attrs[16][4], PicaVertex *v)
{
    VsUnit u;
    float outs[16][4];
    uint64_t map = ((uint64_t)p_regs[0x2BC] << 32) | p_regs[0x2BB];
    uint32_t maxa = p_regs[0x2B9] & 0xFu;
    uint32_t a, oi = 0;
    float slot[32];
    int i;

    memset(&u, 0, sizeof(u));
    for (a = 0; a <= maxa; a++) {
        uint32_t reg = (uint32_t)((map >> (a * 4u)) & 0xFu);
        if (reg < 16u) memcpy(u.in[reg], attrs[a], 4 * sizeof(float));
    }
    p_vs_execute(&u);
    memset(outs, 0, sizeof(outs));
    for (a = 0; a < 16u; a++) {
        if ((p_regs[0x2BD] >> a) & 1u) {
            memcpy(outs[oi], u.out[0][a], 4 * sizeof(float));
            oi++;
        }
    }

    for (i = 0; i < 32; i++) slot[i] = 1.0f;
    {
        uint32_t total = p_regs[0x4F] & 7u;
        for (a = 0; a < total; a++) {
            uint32_t m = p_regs[0x50 + a];
            uint32_t sx = m & 0x1Fu, sy = (m >> 8) & 0x1Fu;
            uint32_t sz = (m >> 16) & 0x1Fu, sw = (m >> 24) & 0x1Fu;
            if (sx < 24u) slot[sx] = outs[a][0];
            if (sy < 24u) slot[sy] = outs[a][1];
            if (sz < 24u) slot[sz] = outs[a][2];
            if (sw < 24u) slot[sw] = outs[a][3];
        }
    }
    for (i = 0; i < 4; i++) v->pos[i] = slot[i];
    for (i = 0; i < 4; i++) v->color[i] = slot[8 + i];
    v->uv[0][0] = slot[12];
    v->uv[0][1] = slot[13];
    v->uv[1][0] = slot[14];
    v->uv[1][1] = slot[15];
    v->uv[2][0] = slot[22];       /* Azahar output_vertex.cpp: TEXCOORD2 22/23
                                     (16 ist TEXCOORD0_W, 18..20 ist VIEW) */
    v->uv[2][1] = slot[23];
    for (i = 0; i < 4; i++) {
        float c = v->color[i];
        if (c < 0.0f) c = -c;             /* Azahar: abs + saturate */
        if (c > 1.0f) c = 1.0f;
        v->color[i] = c;
    }
}

static void p_vertex_from_attrs(const float attrs[16][4], PicaVertex *v, int use_shader)
{
    if (use_shader) {
        p_vs_process(attrs, v);
    } else {
        memcpy(v->pos, attrs[0], 4 * sizeof(float));
        v->uv[0][0] = attrs[1][0];
        v->uv[0][1] = attrs[1][1];
        v->uv[1][0] = attrs[1][0];
        v->uv[1][1] = attrs[1][1];
        v->uv[2][0] = attrs[1][0];
        v->uv[2][1] = attrs[1][1];
        v->color[0] = v->color[1] = v->color[2] = v->color[3] = 1.0f;
    }
    g_vs_last_vertex = *v;
    g_vs_have_last = 1;
}

static void p_emit_tri(ArmCpu *c, const PicaVertex *a, const PicaVertex *b,
                       const PicaVertex *d)
{
    /* Clip -> Viewport -> Rasterisierung + TEV + Farbziel (Azahar
       sw_rasterizer.cpp). Keine NDC-Heuristik mehr. */
    p_raster_tri(c, a, b, d);
}

static uint32_t p_read8m(ArmCpu *c, uint32_t addr)
{
    addr &= 0x1FFFFFFFu;
    return (addr < c->mem_size) ? c->mem[addr] : 0u;
}

static uint32_t p_read16m(ArmCpu *c, uint32_t addr)
{
    uint16_t v = 0;
    addr &= 0x1FFFFFFFu;
    if (addr + 2u <= c->mem_size) memcpy(&v, c->mem + addr, 2);
    return v;
}

/* Index-Puffer 0x227 (Offset relativ zur Attributbasis, Bit31 = u16). */
static uint32_t p_index_vertex(ArmCpu *c, uint32_t i)
{
    uint32_t base = gpu_pica_phys_to_guest(((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u);
    uint32_t off = p_regs[0x227] & 0x0FFFFFFFu;
    int u16 = (int)((p_regs[0x227] >> 31) & 1u);
    if (u16) return p_read16m(c, base + off + i * 2u);
    return p_read8m(c, base + off + i);
}

/* ---- Default-Attribut-/Immediate-Modus (0x232, Azahar SubmitImmediate) ---- */
static float g_vs_imm_attr[16][4];
static unsigned g_vs_imm_cur;
static uint32_t g_vs_imm_pack[4];
static unsigned g_vs_imm_pack_n;
static PicaVertex g_imm_tri[3];
static unsigned g_imm_n;

static void p_vs_draw_immediate(ArmCpu *c)
{
    PicaVertex v;
    p_vertex_from_attrs(g_vs_imm_attr, &v, g_vs_code_words > 0);
    g_imm_tri[g_imm_n++] = v;
    g_vs_imm_cur = 0;
    if (g_imm_n >= 3) {
        p_emit_tri(c, &g_imm_tri[0], &g_imm_tri[1], &g_imm_tri[2]);
        g_imm_n = 0;
    }
}

static int p_draw_arrays(ArmCpu *c, int indexed)
{
    static int plog = -1;
    static unsigned draws = 0;
    uint32_t nv = p_regs[0x228];
    uint32_t voff = p_regs[0x22a];
    int topo;
    PicaVertex tri[3];
    unsigned tri_n = 0;
    uint32_t i;
    int use_shader = (g_vs_code_words > 0);

    if (plog < 0) { const char *e = getenv("HLE_PICA_LOG"); plog = (e && *e) ? atoi(e) : 0; }
    if (nv == 0) return 0;
    if (nv > 0x10000u) nv = 0x10000u;
    g_pica_draws++;
    if (plog > 0 && draws < (unsigned)plog) {
        uint32_t base = ((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
        printf("[pica] draw#%u nv=%u voff=%u base=%08X attr0=%08X attr1=%08X attr2=%08X "
               "idx=%d tex0=%08X fmt=%08X vs=%d\n", draws, nv, voff, base,
               p_regs[0x200], p_regs[0x201], p_regs[0x202], indexed,
               p_regs[0x81], p_regs[0x8e], use_shader);
        printf("[pica] loaders %08X %08X %08X / %08X %08X %08X\n",
               p_regs[0x203], p_regs[0x204], p_regs[0x205],
               p_regs[0x206], p_regs[0x207], p_regs[0x208]);
        for (uint32_t v = 0; v < nv && v < 3u; v++) {
            float a[16][4];
            p_load_attrs(c, indexed ? p_index_vertex(c, v) : (voff + v), a);
            printf("[pica]  v%u:", v);
            for (int k = 0; k < 4; k++)
                printf(" a%d=(%.3f,%.3f,%.3f,%.3f)", k, a[k][0], a[k][1], a[k][2], a[k][3]);
            printf("\n");
        }
        fflush(stdout);
    }

    topo = (int)((p_regs[0x25E] >> 8) & 3u);
    if (topo == 3) topo = 0;            /* Shader-Topologie: als Liste */
    for (i = 0; i < nv; i++) {
        uint32_t vert = indexed ? p_index_vertex(c, i) : (i + voff);
        float attrs[16][4];
        p_load_attrs(c, vert, attrs);
        p_vertex_from_attrs(attrs, &tri[tri_n++], use_shader);
        if (tri_n == 3u) {
            p_emit_tri(c, &tri[0], &tri[1], &tri[2]);
            if (topo == 1) {          /* Strip */
                tri[0] = tri[1];
                tri[1] = tri[2];
                tri_n = 2;
            } else if (topo == 2) {   /* Fan */
                tri[1] = tri[2];
                tri_n = 2;
            } else {
                tri_n = 0;
            }
        }
    }
    draws++;
    return 1;
}

static void p_unpack_f24(uint32_t w0, uint32_t w1, uint32_t w2, float out[4])
{
    uint32_t x = w2 & 0xFFFFFFu;
    uint32_t y = ((w1 & 0xFFFFu) << 8) | ((w2 >> 24) & 0xFFu);
    uint32_t z = ((w0 & 0xFFu) << 16) | ((w1 >> 16) & 0xFFFFu);
    uint32_t w = w0 >> 8;
    out[0] = p_f24_to_float(x);
    out[1] = p_f24_to_float(y);
    out[2] = p_f24_to_float(z);
    out[3] = p_f24_to_float(w);
}

/* Float-Uniform-Queue (Azahar WriteUniformFloatReg): 3 Woerter float24
   oder 4 Woerter float32 je Vec4. Index steht in 0x2C0 (Bit31 = Format),
   wird nach jedem vollstaendigen Vec4 um eins erhoeht (7 Bit). */
static void p_vs_uniform_push(uint32_t word)
{
    int is32 = (int)((p_regs[0x2C0] >> 31) & 1u);
    unsigned need = is32 ? 4u : 3u;
    unsigned idx;

    g_vs_pack[g_vs_pack_n++] = word;
    if (g_vs_pack_n < need) return;

    idx = p_regs[0x2C0] & 0x7Fu;
    if (idx < 96u) {
        int i;
        if (is32) {
            for (i = 0; i < 4; i++) {
                float f;
                memcpy(&f, &g_vs_pack[i], 4);
                g_vs_float[idx][3 - i] = f;
            }
        } else {
            p_unpack_f24(g_vs_pack[0], g_vs_pack[1], g_vs_pack[2], g_vs_float[idx]);
        }
        p_regs[0x2C0] = (p_regs[0x2C0] & ~0x7Fu) | ((idx + 1u) & 0x7Fu);
    }
    g_vs_pack_n = 0;
}

/* Default-Attribute/Immediate: Azahar PicaCore::SubmitImmediate. */
static void p_vs_submit_immediate(ArmCpu *c, uint32_t word)
{
    unsigned idx = p_regs[0x232] & 0xFu;
    float a[4];

    g_vs_imm_pack[g_vs_imm_pack_n++] = word;
    if (g_vs_imm_pack_n < 3u) return;
    p_unpack_f24(g_vs_imm_pack[0], g_vs_imm_pack[1], g_vs_imm_pack[2], a);
    g_vs_imm_pack_n = 0;

    if (idx < 0xFu) {
        memcpy(g_vs_def_attr[idx], a, sizeof(a));
        p_regs[0x232] = idx + 1u;
        return;
    }
    memcpy(g_vs_imm_attr[g_vs_imm_cur], a, sizeof(a));
    if (g_vs_imm_cur < (p_regs[0x242] & 0xFu)) {
        g_vs_imm_cur++;
        return;
    }
    p_vs_draw_immediate(c);
}

/* Spezialregister des Vertex-Shaders (Azahar HandleSpecialReg). Wird nach
   dem maskierten Register-Schreiben mit dem Rohwert aufgerufen. */
static void p_vs_reg_write(ArmCpu *c, uint32_t id, uint32_t value)
{
    if (id == 0x2B0u) {
        unsigned i;
        for (i = 0; i < 16u; i++) g_vs_bool[i] = (unsigned char)((value >> i) & 1u);
    } else if (id >= 0x2B1u && id <= 0x2B4u) {
        memcpy(g_vs_int[id - 0x2B1u], &value, 4);
    } else if (id >= 0x2C1u && id <= 0x2C8u) {
        p_vs_uniform_push(value);
    } else if (id >= 0x2CCu && id <= 0x2D3u) {
        uint32_t off = p_regs[0x2CB];
        if (off < VS_PROG_WORDS) {
            g_vs_code[off] = value;
            if (off + 1u > g_vs_code_words) g_vs_code_words = off + 1u;
            p_regs[0x2CB] = off + 1u;
        }
    } else if (id >= 0x2D6u && id <= 0x2DDu) {
        uint32_t off = p_regs[0x2D5];
        if (off < VS_SWZ_WORDS) {
            g_vs_swz[off] = value;
            p_regs[0x2D5] = off + 1u;
        }
    } else if (id == 0x232u) {
        g_vs_imm_pack_n = 0;
        g_vs_imm_cur = 0;
    } else if (id >= 0x233u && id <= 0x235u) {
        p_vs_submit_immediate(c, value);
    }
}

/* ---- Test-/Diagnosezugriff (keine Spielsemantik) ---- */
static void p_vs_reset_draw_state(void)
{
    g_vs_imm_pack_n = 0;
    g_vs_imm_cur = 0;
    g_imm_n = 0;
    g_vs_have_last = 0;
}

float gpu_pica_vs_uniform(int index, int comp)
{
    if (index < 0 || index >= 96 || comp < 0 || comp >= 4) return 0.0f;
    return g_vs_float[index][comp];
}

int gpu_pica_vs_active(void) { return g_vs_code_words > 0; }

int gpu_pica_last_vertex(float *pos, float *color, float *uv)
{
    if (!g_vs_have_last) return 0;
    if (pos) memcpy(pos, g_vs_last_vertex.pos, 4 * sizeof(float));
    if (color) memcpy(color, g_vs_last_vertex.color, 4 * sizeof(float));
    if (uv) memcpy(uv, g_vs_last_vertex.uv, 2 * sizeof(float));
    return 1;
}

/* Laufzustand einer Kommandoliste. Ein Sprung ueber 0x23C/0x23D ersetzt in
 * Azahar die aktuelle Liste (cmd_list.Reset), der Rest der alten Liste wird
 * also nicht mehr ausgefuehrt. */
typedef struct {
    int ops;                 /* verarbeitete Register-Schreibungen */
    int stop;                /* 0x010 Finalize mit irq_autostop */
    uint32_t jump_addr;      /* physische Adresse des Ziel-Kanals */
    uint32_t jump_size;
} PicaCtx;

static int p_any_byte_match(uint32_t a, uint32_t b)
{
    return ((a & 0xFFu) == (b & 0xFFu)) ||
           (((a >> 8) & 0xFFu) == ((b >> 8) & 0xFFu)) ||
           (((a >> 16) & 0xFFu) == ((b >> 16) & 0xFFu)) ||
           (((a >> 24) & 0xFFu) == ((b >> 24) & 0xFFu));
}

static void p_write_reg(ArmCpu *c, uint32_t id, uint32_t value, uint32_t mask,
                        PicaCtx *ctx)
{
    uint32_t old;
    if (id >= PICA_NREGS) return;   /* Azahar: OOB-Schreibzugriffe sind No-ops */
    old = p_regs[id];
    for (int b = 0; b < 4; b++)
        if (mask & (1u << b))
            old = (old & ~(0xFFu << (b * 8))) | (value & (0xFFu << (b * 8)));
    p_regs[id] = old;
    ctx->ops++;

    p_vs_reg_write(c, id, value);
    if (id >= 0xE8u && id <= 0xEFu) p_fog_lut_write(value);

    if (id == 0x010u) {             /* Finalize / irq_request */
        /* Azahar: every_byte_match(irq_request, irq_compare) -> P3D; bei
           irq_autostop endet die Liste hier. */
        if (p_any_byte_match(p_regs[0x010], p_regs[0x020])) {
            if (p_irq_handler) p_irq_handler(5);
            if (p_regs[0x034] & 1u) ctx->stop = 1;
        }
    } else if (id == 0x22Eu) {
        p_draw_arrays(c, 0);
    } else if (id == 0x22Fu) {
        p_draw_arrays(c, 1);
    } else if (id == 0x23Cu || id == 0x23Du) {
        uint32_t ch = id - 0x23Cu;
        uint32_t trigger = p_regs[id];
        uint32_t paddr, sz;
        p_regs[id] = 0;             /* Azahar: trigger wird nach dem Lauf geloescht */
        if (trigger == 0u) return;
        paddr = (p_regs[0x23Au + ch] & 0x0FFFFFFFu) * 8u;
        sz = (p_regs[0x238u + ch] & 0x000FFFFFu) * 8u;
        if (paddr == 0u || sz == 0u
                || (uint64_t)gpu_pica_phys_to_guest(paddr) + sz > c->mem_size) {
            /* Azahar GPU::SubmitCmdList: ungueltige Liste meldet P3D. */
            if (p_irq_handler) p_irq_handler(5);
            return;
        }
        ctx->jump_addr = paddr;
        ctx->jump_size = sz;
    }
}

static void p_run_list(ArmCpu *c, uint32_t addr, uint32_t size, PicaCtx *ctx,
                       int depth)
{
    uint32_t a = addr & 0xFFFFFFFFu;   /* bereits Gast-Linearspeicher */
    uint32_t words = size / 4u;
    uint32_t i = 0;

    if (depth > 8 || words < 2u) return;   /* Schutz gegen Trigger-Rekursion */
    while (i + 1u < words) {
        uint32_t value, hdr, id, mask, extra, group;
        if ((i & 1u) != 0u) i++;          /* 8-Byte-Ausrichtung (Pad-Wort) */
        if (i + 1u >= words) break;
        value = p_read32(c, a + i * 4u);
        hdr = p_read32(c, a + (i + 1u) * 4u);
        i += 2u;
        id = hdr & 0xFFFFu;
        mask = (hdr >> 16) & 0xFu;
        extra = (hdr >> 20) & 0xFFu;
        group = (hdr >> 31) & 1u;

        p_write_reg(c, id, value, mask, ctx);
        if (ctx->stop || ctx->jump_addr) break;

        for (uint32_t k = 0; k < extra; k++) {
            uint32_t ev, eid;
            if (i >= words) break;
            ev = p_read32(c, a + i * 4u);
            eid = group ? (id + 1u + k) : id;
            i++;
            p_write_reg(c, eid, ev, mask, ctx);
            if (ctx->stop || ctx->jump_addr) break;
            /* Nach einem ungeraden Record folgt ein Pad-Wort; das erledigt
               die Ausrichtung am Schleifenkopf. */
        }
    }

    if (ctx->jump_addr) {
        uint32_t ja = ctx->jump_addr;
        uint32_t js = ctx->jump_size;
        ctx->jump_addr = 0;
        ctx->jump_size = 0;
        p_run_list(c, gpu_pica_phys_to_guest(ja), js, ctx, depth + 1);
    }
}

int gpu_run_pica_list(ArmCpu *c, uint32_t addr, uint32_t size)
{
    PicaCtx ctx;

    memset(&ctx, 0, sizeof(ctx));
    p_run_list(c, gpu_pica_phys_to_guest(addr), size, &ctx, 0);
    g_pica_lists++;
    return ctx.ops;
}
