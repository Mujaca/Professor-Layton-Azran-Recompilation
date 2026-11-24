/* PICA200-Kommandolisten-Ausfuehrer + Software-Rasterizer (Teilmenge).
 *
 * gpu_run_pica_list() liest Paare (value, header) wie die echte GPU
 * (docs/REFERENCE.md, PICA200) und fuehrt Draws (0x22E/0x22F) mit einem
 * heuristischen Rasterizer aus: Position = Attribut 0, UV = erstes
 * 2-Komponenten-Attribut, Textur 0 aus 0x080..0x08E.
 *
 * Fehlt noch (ROADMAP M5): Vertex-Shader (0x2C0..), TEV (0x0C0..),
 * Tiefen-/Alpha-/Blend-Zustand, Index-Puffer, Ausgabe in das vom Spiel
 * konfigurierte Farbziel (0x11C..).
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

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

static float p_attr_comp(ArmCpu *c, uint32_t addr, int fmt)
{
    uint32_t o = addr & 0x1FFFFFFFu;
    if (fmt == 3) { float f = 0.0f; if (o + 4u <= c->mem_size) memcpy(&f, c->mem + o, 4); return f; }
    if  (fmt == 2) { int16_t s = 0; if (o + 2u <= c->mem_size) memcpy(&s, c->mem + o, 2); return (float)s; }
    if (fmt == 1) { uint8_t u = 0; if (o + 1u <= c->mem_size) u = c->mem[o]; return (float)u; }
    { int8_t b = 0; if (o + 1u <= c->mem_size) b = (int8_t)c->mem[o]; return (float)b; }
}

/* Liest die Vertex-Attribute fuer Vertex v und schreibt sie nach out[attr][0..3]. */
static void p_load_attrs(ArmCpu *c, uint32_t vert, float out[16][4])
{
    int fmt[16], ncomp[16];
    uint32_t src[16], stride[16];
    /* 0x200: base_address (bits 1..28) * 16 = physische Adresse. */
    uint32_t base = gpu_pica_phys_to_guest(((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u);
    uint32_t maxattr = (p_regs[0x202] >> 28) & 0xFu;
    for (int i = 0; i < 16; i++) { fmt[i] = 3; ncomp[i] = 0; src[i] = 0; stride[i] = 0; }
    for (int i = 0; i < 8; i++) {
        uint32_t b = (p_regs[0x201] >> (i * 4)) & 0xFu;
        fmt[i] = b & 3; ncomp[i] = ((b >> 2) & 3) + 1;
    }
    for (int i = 0; i < 4; i++) {
        uint32_t b = (p_regs[0x202] >> (i * 4)) & 0xFu;
        fmt[8 + i] = b & 3; ncomp[8 + i] = ((b >> 2) & 3) + 1;
    }
    for (int l = 0; l < 12; l++) {
        /* 12 Loader zu je 3 Woertern ab 0x203 (Azahar regs_internal.h:
           vertex_attributes 0x200 .. index_array 0x227). */
        uint32_t w0 = p_regs[0x203 + l * 3];
        uint32_t w1 = p_regs[0x204 + l * 3];
        uint32_t w2 = p_regs[0x205 + l * 3];
        uint32_t off = w0 & 0x0FFFFFFFu;
        uint32_t bcount = (w2 >> 16) & 0xFFu;
        uint32_t cc = (w2 >> 28) & 0xFu;
        uint32_t cur = off;
        for (uint32_t k = 0; k < cc && k < 12; k++) {
            uint32_t ai = (k < 8) ? ((w1 >> (k * 4)) & 0xFu) : ((w2 >> ((k - 8) * 4)) & 0xFu);
            if (ai < 12) {
                src[ai] = cur; stride[ai] = bcount;
                cur += (uint32_t)ncomp[ai] * ((fmt[ai] == 3) ? 4u : (fmt[ai] == 2) ? 2u : 1u);
            } else if (ai < 16) {
                cur += (ai - 11) * 4u;
            }
        }
    }
    for (int i = 0; i <= (int)maxattr && i < 16; i++) {
        for (int k = 0; k < 4; k++) out[i][k] = (k == 3) ? 1.0f : 0.0f;
        if (src[i] == 0 && stride[i] == 0 && ncomp[i] == 0) continue;
        uint32_t a = base + src[i] + stride[i] * vert;
        int eb = (fmt[i] == 3) ? 4 : (fmt[i] == 2) ? 2 : 1;
        for (int k = 0; k < ncomp[i] && k < 4; k++)
            out[i][k] = p_attr_comp(c, a + (uint32_t)(k * eb), fmt[i]);
        for (int k = ncomp[i]; k < 4; k++) out[i][k] = (k == 3) ? 1.0f : 0.0f;
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
                         float x1, float y1, float u1, float v1,
                         float x2, float y2, float u2, float v2,
                         uint32_t col)
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
            uint32_t rgb = ok ? tex : col;
            if (ok) {
                put_px_blend(c, x, y, rgb & 0xFFFFFFu, (tex >> 24) & 0xFFu);
            } else {
                put_px(c, x, y, rgb & 0xFFFFFFu);
            }
        }
    }
}

static int p_draw_arrays(ArmCpu *c, int indexed)
{
    static int plog = -1;
    static unsigned draws = 0;
    uint32_t nv = p_regs[0x228];
    uint32_t voff = p_regs[0x22a];
    if (plog < 0) { const char *e = getenv("HLE_PICA_LOG"); plog = (e && *e) ? atoi(e) : 0; }
    if (nv == 0) return 0;
    if (nv > 0x10000u) nv = 0x10000u;
    g_pica_draws++;
    if (plog > 0 && draws < (unsigned)plog) {
        uint32_t base = ((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
        printf("[pica] draw#%u nv=%u voff=%u base=%08X attr0=%08X attr1=%08X attr2=%08X "
               "idx=%d tex0=%08X fmt=%08X\n", draws, nv, voff, base,
               p_regs[0x200], p_regs[0x201], p_regs[0x202], indexed,
               p_regs[0x81], p_regs[0x8e]);
        for (uint32_t v = 0; v < nv && v < 3u; v++) {
            float a[16][4];
            p_load_attrs(c, voff + v, a);
            printf("[pica]  v%u:", v);
            for (int i = 0; i < 4; i++)
                printf(" a%d=(%.3f,%.3f,%.3f,%.3f)", i, a[i][0], a[i][1], a[i][2], a[i][3]);
            printf("\n");
        }
        fflush(stdout);
    }
    /* Heuristischer 2D-Renderer: Position = Attribut 0, UV = erstes
       Attribut mit 2 Komponenten (1, sonst 2), Farbe = Attribut 2/3.
       Platzhalter bis Vertex-Shader + TEV existieren (ROADMAP M5). */
    {
        {
            for (uint32_t v = 0; v + 2u < nv; v += 3u) {
                float a[3][16][4];
                p_load_attrs(c, voff + v, a[0]);
                p_load_attrs(c, voff + v + 1u, a[1]);
                p_load_attrs(c, voff + v + 2u, a[2]);
                float px[3], py[3], uu[3], vv[3];
                /* NDC nur, wenn ALLE drei Eckpunkte in [-1.6,1.6] liegen.
                   Sonst sind es bereits Pixelkoordinaten (2D-Quad, z. B.
                   (0,0)-(400,240)); sonst wuerde (0,0) als NDC-Mitte landen. */
                int ndc = 1;
                for (int k = 0; k < 3; k++) {
                    float x = a[k][0][0], y = a[k][0][1];
                    if (!(x > -1.6f && x < 1.6f && y > -1.6f && y < 1.6f))
                        ndc = 0;
                }
                for (int k = 0; k < 3; k++) {
                    float x = a[k][0][0], y = a[k][0][1];
                    if (ndc) {
                        px[k] = (x + 1.0f) * 0.5f * (float)g_fb_w;
                        py[k] = (1.0f - y) * 0.5f * (float)g_fb_h;
                    } else { px[k] = x; py[k] = y; }
                    uu[k] = a[k][1][0]; vv[k] = a[k][1][1];
                }
                p_raster_tri(c, px[0], py[0], uu[0], vv[0],
                             px[1], py[1], uu[1], vv[1],
                             px[2], py[2], uu[2], vv[2], 0x00FFFFFFu);
            }
        }
    }
    draws++;
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
