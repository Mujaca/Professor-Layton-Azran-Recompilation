/* PICA200-Kommandolisten-Ausfuehrer + Software-Rasterizer (Teilmenge).
 *
 * gpu_run_pica_list() liest Paare (value, header) wie die echte GPU
 * (docs/REFERENCE.md, PICA200) und fuehrt Draws (0x22E/0x22F) mit einem
 * heuristischen Rasterizer aus: Position = Attribut 0, UV = erstes
 * 2-Komponenten-Attribut, Textur 0 aus 0x080..0x08E.
 *
 * Vertex-Pipeline: Vertex-Shader-Interpreter (0x2B0..0x2DF: Uniforms,
 * Programm, Swizzle, Arithmetik, CALL/IF/LOOP/BREAK) und Attribute-Loader
 * (0x200..0x227 inkl. Defaults, Index-Puffer 0x22F, Immediate 0x232) nach
 * Azahar shader_interpreter.cpp/vertex_loader.cpp; Ausgangs-Mapping
 * 0x04F..0x056. Ohne geladenes Programm bleibt die alte Draw-Heuristik.
 * Fehlt noch (ROADMAP M5): TEV (0x0C0..), Viewport/Clip/Depth,
 * Blend-Zustand, Ausgabe in das vom Spiel konfigurierte Farbziel (0x11C..).
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

/* Textur 0 (falls aktiv) als RGBA-Quelle. Format aus Reg 0x8E,
   Adresse aus Reg 0x85, Groesse aus Reg 0x82. */
static uint32_t p_texel(ArmCpu *c, float u, float v, int *ok)
{
    uint32_t cfg = p_regs[0x82];
    uint32_t fmt = p_regs[0x8E] & 0xFu;
    /* 0x85: Adresse >> 3; PICA-Adressen sind physisch. */
    uint32_t addr = gpu_pica_phys_to_guest((p_regs[0x85] & 0x0FFFFFFFu) * 8u);
    int w = (int)((cfg >> 16) & 0x7FFu);
    int h = (int)(cfg & 0x7FFu);
    /* Enable steht in main_config (0x80) Bit 0, nicht in 0x81. */
    if (!(p_regs[0x80] & 1u) || w <= 0 || h <= 0) { *ok = 0; return 0; }
    {
        int x = (int)(u * (float)w);
        int y = (int)(v * (float)h);
        if (x < 0) x = 0; if (x >= w) x = w - 1;
        if (y < 0) y = 0; if (y >= h) y = h - 1;
        uint32_t o = (addr + (uint32_t)(y * w + x) * 2u) & 0x1FFFFFFFu;
        uint32_t r = 255, g = 255, b = 255, a = 255;
        switch (fmt) {
        case 0: /* RGBA8 */
            o = (addr + (uint32_t)(y * w + x) * 4u) & 0x1FFFFFFFu;
            if (o + 4u <= c->mem_size) {
                r = c->mem[o]; g = c->mem[o + 1]; b = c->mem[o + 2]; a = c->mem[o + 3];
            }
            break;
        case 1: /* RGB8 */
            o = (addr + (uint32_t)(y * w + x) * 3u) & 0x1FFFFFFFu;
            if (o + 3u <= c->mem_size) { r = c->mem[o]; g = c->mem[o + 1]; b = c->mem[o + 2]; }
            break;
        case 3: /* RGB565 */
            if (o + 2u <= c->mem_size) {
                uint32_t v = c->mem[o] | ((uint32_t)c->mem[o + 1] << 8);
                r = ((v >> 11) & 0x1F) * 255 / 31;
                g = ((v >> 5) & 0x3F) * 255 / 63;
                b = (v & 0x1F) * 255 / 31;
            }
            break;
        case 4: /* RGBA4 */
            if (o + 2u <= c->mem_size) {
                uint32_t v = c->mem[o] | ((uint32_t)c->mem[o + 1] << 8);
                r = ((v >> 12) & 0xF) * 17; g = ((v >> 8) & 0xF) * 17;
                b = ((v >> 4) & 0xF) * 17; a = (v & 0xF) * 17;
            }
            break;
        case 5: /* IA8 */
            if (o + 2u <= c->mem_size) { r = g = b = c->mem[o]; a = c->mem[o + 1]; }
            break;
        case 7: /* I8 */
            if (o + 1u <= c->mem_size) { r = g = b = c->mem[o]; }
            break;
        case 8: /* A8 */
            if (o + 1u <= c->mem_size) { a = c->mem[o]; }
            break;
        default:
            break;
        }
        *ok = 1;
        return (r & 0xFFu) | ((g & 0xFFu) << 8) | ((b & 0xFFu) << 16) | ((a & 0xFFu) << 24);
    }
}

static void p_raster_tri(ArmCpu *c, float x0, float y0, float u0, float v0,
                         float c0r, float c0g, float c0b, float c0a,
                         float x1, float y1, float u1, float v1,
                         float c1r, float c1g, float c1b, float c1a,
                         float x2, float y2, float u2, float v2,
                         float c2r, float c2g, float c2b, float c2a)
{
    float d = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
    if (d == 0.0f) return;
    int minx = (int)(x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2));
    int maxx = (int)(x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2));
    int miny = (int)(y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2));
    int maxy = (int)(y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2));
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= g_fb_w) maxx = g_fb_w - 1;
    if (maxy >= g_fb_h) maxy = g_fb_h - 1;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            float px = x + 0.5f, py = y + 0.5f;
            float a = (y1 - y2) * (px - x2) + (x2 - x1) * (py - y2);
            float b = (y2 - y0) * (px - x2) + (x0 - x2) * (py - y2);
            float w0 = a / d, w1 = b / d, w2 = 1.0f - w0 - w1;
            if (w0 < 0.0f || w1 < 0.0f || w2 < 0) continue;
            float u = w0 * u0 + w1 * u1 + w2 * u2;
            float v = w0 * v0 + w1 * v1 + w2 * v2;
            int ok = 0;
            uint32_t tex = p_texel(c, u, v, &ok);
            if (ok) {
                put_px_blend(c, x, y, tex & 0xFFFFFFu, (tex >> 24) & 0xFFu);
            } else {
                /* VS-Farbe (COLOR_R..A) linear interpolieren, clampen. */
                float cr = w0 * c0r + w1 * c1r + w2 * c2r;
                float cg = w0 * c0g + w1 * c1g + w2 * c2g;
                float cb = w0 * c0b + w1 * c1b + w2 * c2b;
                float ca = w0 * c0a + w1 * c1a + w2 * c2a;
                uint32_t rgb;
                if (cr < 0.0f) cr = 0.0f; if (cr > 1.0f) cr = 1.0f;
                if (cg < 0.0f) cg = 0.0f; if (cg > 1.0f) cg = 1.0f;
                if (cb < 0.0f) cb = 0.0f; if (cb > 1.0f) cb = 1.0f;
                if (ca < 0.0f) ca = 0.0f; if (ca > 1.0f) ca = 1.0f;
                rgb = ((uint32_t)(cr * 255.0f)) |
                      (((uint32_t)(cg * 255.0f)) << 8) |
                      (((uint32_t)(cb * 255.0f)) << 16);
                                                                            put_px_blend(c, x, y, rgb, (uint32_t)(ca * 255.0f));
            }
        }
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
    float pos[4];
    float color[4];
    float uv[2];
} PicaVertex;

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
    v->uv[0] = slot[12];
    v->uv[1] = slot[13];
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
        v->uv[0] = attrs[1][0];
        v->uv[1] = attrs[1][1];
        v->color[0] = v->color[1] = v->color[2] = v->color[3] = 1.0f;
    }
    g_vs_last_vertex = *v;
    g_vs_have_last = 1;
}

static void p_emit_tri(ArmCpu *c, const PicaVertex *a, const PicaVertex *b,
                       const PicaVertex *d)
{
    const PicaVertex *src[3];
    float px[3], py[3];
    int k, ndc = 1;

    src[0] = a; src[1] = b; src[2] = d;
    for (k = 0; k < 3; k++) {
        float x = src[k]->pos[0], y = src[k]->pos[1];
        if (!(x > -1.6f && x < 1.6f && y > -1.6f && y < 1.6f)) ndc = 0;
    }
    for (k = 0; k < 3; k++) {
        float x = src[k]->pos[0], y = src[k]->pos[1];
        if (ndc) {
            px[k] = (x + 1.0f) * 0.5f * (float)g_fb_w;
            py[k] = (1.0f - y) * 0.5f * (float)g_fb_h;
        } else {
            px[k] = x;
            py[k] = y;
        }
    }
    p_raster_tri(c, px[0], py[0], src[0]->uv[0], src[0]->uv[1],
                 src[0]->color[0], src[0]->color[1], src[0]->color[2], src[0]->color[3],
                 px[1], py[1], src[1]->uv[0], src[1]->uv[1],
                 src[1]->color[0], src[1]->color[1], src[1]->color[2], src[1]->color[3],
                 px[2], py[2], src[2]->uv[0], src[2]->uv[1],
                 src[2]->color[0], src[2]->color[1], src[2]->color[2], src[2]->color[3]);
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
