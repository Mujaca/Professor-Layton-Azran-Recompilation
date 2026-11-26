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
 * Wrap/Border; keine Filterung wie Azahar, ETC1-Subtiles nach rohen
 * Kachelkoordinaten; nur Einheit 0 beachtet den Typ: Projection2D,
 * Cube/ShadowCube (Face-Adressen 0x86..0x8A), Shadow2D inkl. Bias,
 * TEXCOORD0_W 0x8B; TEXCOORD2 = 22/23), prozedurale Textur (Einheit 3,
 * 0x0A8..0x0AD/0x0AF/0xB0..0xB7 nach Azahar sw_proctex.cpp: LUTs,
 * Noise, Shift, Clamp, Combiner, separate Alpha, Filter), Fragment-Lighting
 * 0x140..0x1D9 nach Azahar sw_lighting.cpp (Lichtquellen, Distanz-/Spot-Daempfung,
 * Fresnel, Bump-Mapping, Clamp, LUT-Upload 0x1C5/0x1C8..0x1CF) in die
 * TEV-Quellen Primary/SecondaryFragment, Output-Merger 0x100..0x107
 * (Blending, LogicOp, Alpha-Test) und Farb-/Tiefenziel 0x110..0x11E
 * (Kanalmasken, RGBA8/RGB8/RGB5A1/RGB565/RGBA4, Depth-Test/Write,
 * Stencil, D16/D24/D24S8) sowie Fog (0xE0/0xE1/0xE6/0xE8..0xEF, Azahar
 * sw_rasterizer.cpp WriteFog). Noch offen (ROADMAP M5): Mipmaps (im
 * Azahar-Software-Renderer ebenfalls nicht umgesetzt).
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <fenv.h>

/* Host-Threads fuer das parallele Rasterisieren grosser Dreiecke. */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "arm_cpu.h"
#include "hw_d3d11.h"

#define GPU_FB_DEFAULT 0x1F000000u  /* Port-Fenster liest hier */

static uint32_t g_fb = GPU_FB_DEFAULT;
static uint32_t g_clear = 0x000000u;
static int g_fb_w = 400;
static int g_fb_h = 240;

/* Determinismus-Zaehler: abgeschlossene Listen/Submits und Draws. */
static volatile unsigned long long g_pica_lists = 0;
static volatile unsigned long long g_pica_draws = 0;
/* Diagnose (HLE_GTIME): Arbeitsvolumen des Software-Rasterizers. Reine
 * Zaehler, kein Gastzustand. */
static volatile unsigned long long g_frag_n = 0;   /* schattierte Fragmente */
static volatile unsigned long long g_tex_n = 0;    /* aktive Textursamples */
static volatile unsigned long long g_lit_n = 0;    /* Fragment-Lighting */
static volatile unsigned long long g_fb_n = 0;     /* Farbziel-Paare */

unsigned long long gpu_pica_list_count(void) { return g_pica_lists; }
unsigned long long gpu_pica_draw_count(void) { return g_pica_draws; }
void gpu_pica_work_stats(unsigned long long *frag, unsigned long long *tex,
                         unsigned long long *lit, unsigned long long *fb) {
    if (frag) *frag = g_frag_n;
    if (tex) *tex = g_tex_n;
    if (lit) *lit = g_lit_n;
    if (fb) *fb = g_fb_n;
}

void gpu_set_fb(uint32_t addr) { if (addr != 0) g_fb = addr; }
uint32_t gpu_get_fb(void) { return g_fb; }
void gpu_set_fb_size(uint32_t addr, int w, int h) {
    if (addr != 0) g_fb = addr;
    if (w > 0) g_fb_w = w;
    if (h > 0) g_fb_h = h;
}
int gpu_get_fb_width(void) { return g_fb_w; }
int gpu_get_fb_height(void) { return g_fb_h; }

/* Ohne konfiguriertes Farbziel (0x11D) und ohne Vertex-Programm wird nichts
 * gezeichnet; es gibt keinen Host-Puffer-/Attribut-Fallback mehr (Azahar
 * rendert ausschliesslich ueber PICA-Register). */

/* Renderer-Auswahl nur per Kommandozeile (--renderer hw in runtime/main.c).
 * Ohne Schalter bleibt der Software-Rasterizer allein aktiv und bitgleich. */
static int g_hw_render = 0;

int gpu_pica_set_render_hw(int on)
{
    if (on && !hw_init(NULL)) {
        printf("[hw] Initialisierung fehlgeschlagen, Software-Pfad bleibt aktiv\n");
        return 0;
    }
    g_hw_render = on ? 1 : 0;
    hw_invalidate_guest();
    return 1;
}

int gpu_pica_render_hw(void) { return g_hw_render; }

/* Gast-Host-Schreibzugriffe ausserhalb der PICA-Listen (Y2R, DisplayTransfer,
 * TextureCopy, MemoryFill) koennen Farb-/Tiefenziel oder Texturdaten
 * veraendern. Der Hardware-Pfad muss seinen Zielzustand dann aus dem Gast
 * neu laden, sonst rendert er auf veralteten Bytes. */
void gpu_pica_invalidate_guest(void)
{
    if (g_hw_render) hw_invalidate_guest();
}

/* Nur Texturen im Gastbereich [offset, offset+bytes) verwerfen. offset ist
 * eine Gast-Linearadresse wie in HwTexture.offset; Aufrufer sind die
 * GX-Schreiboperationen MemoryFill/DisplayTransfer/TextureCopy und Y2R. */
void gpu_pica_invalidate_tex_range(uint32_t offset, uint32_t bytes)
{
    if (g_hw_render) hw_invalidate_texture_range(offset, bytes);
}

/* Diagnose: Textur-Uploads und Dekodierzeit des Hardware-Pfads. */
void gpu_pica_texture_stats(unsigned long long *uploads, unsigned long long *decode_us)
{
    if (uploads) *uploads = 0ull;
    if (decode_us) *decode_us = 0ull;
    if (g_hw_render) hw_texture_stats(uploads, decode_us);
}

void gpu_pica_flush_guest(void)
{
    if (g_hw_render) hw_flush_to_guest();
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
    /* gs.input_buffer_config: max_input_attribute_index = 1 (Bits0..3),
       shader_mode = VS (Bits24..31). Azahar setzt diesen Default, weil
       Tales of the Abyss ihn erwartet; GSP initialisiert ihn nicht. */
    [0x289] = 0x000000A1u,
};

/* Fog-LUT (0xE8..0xEF schreiben bei 0xE6 % 128, danach Offset++). */
static uint32_t g_fog_lut[128];

/* Lighting-LUT (Azahar PicaCore::Lighting.luts): 24 Typen a 256 Eintraege,
   gefuellt ueber 0x1C8..0x1CF mit Typ/Index aus 0x1C5. */
#define LIGHT_LUT_COUNT 24
static uint32_t g_light_lut[LIGHT_LUT_COUNT][256];
/* Vorberechnete float-Felder derselben Eintraege (Wert und Differenz).
 * Der Schreibpfad 0x1C8..0x1CF ist der einzige, der die Eintraege aendert;
 * die Umrechnung ist exakt die aus p_light_lut_lookup, nur einmal statt je
 * Fragment. */
static float g_light_lut_fval[LIGHT_LUT_COUNT][256];
static float g_light_lut_fdiff[LIGHT_LUT_COUNT][256];

static void p_vs_reset(void);
static void p_vs_reset_draw_state(void);
static void p_light_draw_invalidate(void);
static void p_proctex_reset(void);

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

/* Generation des Gast-Texturspeichers: jede neue Kommandoliste, MemoryFill,
   HW-Rueckschreiben und Software-Draws (auch Fallback-Dreiecke im HW-Modus)
   koennen Texturbytes geaendert haben. Der D3D11-Textur-Cache
   (runtime/gpu/hw_d3d11.c) merkt sich die Generation je Eintrag und prueft
   bei Abweichung einen Hash der Quellbytes. DisplayTransfer/TextureCopy
   markieren stattdessen nur den beschriebenen Bereich
   (gpu_pica_invalidate_tex_range). */
static uint32_t g_hw_tex_gen = 1u;

void gpu_pica_reset(void)
{
    memset(p_regs, 0, sizeof(p_regs));
    memset(g_fog_lut, 0, sizeof(g_fog_lut));
    memset(g_light_lut, 0, sizeof(g_light_lut));
    memset(g_light_lut_fval, 0, sizeof(g_light_lut_fval));
    memset(g_light_lut_fdiff, 0, sizeof(g_light_lut_fdiff));
    p_regs[0x020] = 0x12345678u;
    p_regs[0x030] = 0xFFFFFFF0u;
    p_regs[0x034] = 0x00000001u;
    p_regs[0x289] = 0x000000A1u;   /* gs: max_input_attribute_index 1, VS */
    p_light_draw_invalidate();
    p_proctex_reset();
    p_vs_reset();
    p_vs_reset_draw_state();
    g_hw_tex_gen++;
}

uint32_t gpu_pica_tex_generation(void)
{
    return g_hw_tex_gen;
}

static void p_hw_tex_gen_bump(void)
{
    g_hw_tex_gen++;
}

void gpu_pica_bump_tex_generation(void)
{
    g_hw_tex_gen++;
}

int gpu_pica_texture_bound(void)
{
    return (p_regs[0x80] & 1u) && (p_regs[0x82] & 0x07FF07FFu) != 0u;
}

static uint32_t p_read32(ArmCpu *c, uint32_t off)
{
    uint32_t v = 0;
    off &= 0x1FFFFFFFu;
    if (off + 4u <= c->mem_size) memcpy(&v, c->mem + off, 4);
    return v;
}

/* ===================== Shader-Zustand (Einheit 0 = VS, 3 = GS) =====
 * Azahar ShaderSetup/ShaderRegs: Die Vertex-Einheit liegt bei 0x2B0..0x2DF,
 * die Geometrie-Einheit (Shader-Unit 3) bei 0x280..0x2AF. Jede Einheit hat
 * eigene Programm-, Swizzle- und Uniform-Ressourcen (je 4096 Woerter,
 * Azahar MAX_PROGRAM_CODE_LENGTH/MAX_SWIZZLE_DATA_LENGTH):
 *   0x2B0/0x280 bool_uniforms (1 Bit je Uniform)
 *   0x2B1../0x281.. int_uniforms[4] (4 Byte je Uniform)
 *   0x2C0/0x290 uniform_setup.index (+ Bit31 = Float32-Format),
 *   0x2C1../0x291.. set_value (gepackte float24/float32-Queue)
 *   0x2CB/0x29B program.offset, 0x2CC../0x29C.. set_word
 *   0x2D5/0x2A5 swizzle_patterns.offset, 0x2D6../0x2A6.. set_word
 *   0x2B9/0x289 input_buffer_config (Bits0..3 max_input_attribute_index,
 *             Bits8..15 input_to_uniform, Bits24..31 shader_mode)
 *   0x2BA/0x28A main_offset, 0x2BB/0x2BC + 0x28B/0x28C Input-Map,
 *   0x2BD/0x28D output_mask
 * Die VS-Register koennen nur die ersten 512 Programmwoerter adressieren
 * (Azahar: "Invalid VS program offset"), die GS-Register alle 4096.
 * Default-Attribute/Immediate des Vertex-Loaders: 0x232 index,
 * 0x233..0x235 set_value.
 */
#define VS_PROG_WORDS 4096u
#define VS_SWZ_WORDS  4096u
#define VS_MAX_STEPS  0x10000u
#define VS_REG_WORDS  512u      /* adressierbares VS-Programmfenster */

typedef struct {
    uint32_t code[VS_PROG_WORDS];
    unsigned code_words;
    uint32_t swz[VS_SWZ_WORDS];
    float f[96][4];             /* uniforms.f */
    unsigned char b[16];        /* uniforms.b */
    unsigned char i[4][4];      /* uniforms.i */
    uint32_t pack[4];           /* Uniform-Packing-Queue */
    unsigned pack_n;
} PicaShader;

static PicaShader g_vs;         /* Shader-Einheit 0 (Vertex-Shader) */
static PicaShader g_gs;         /* Shader-Einheit 3 (VS oder GS) */
/* Azahar GeometryShaderUnit ist zustandsbehaftet: Ausgangsregister und
 * Ausgangsbank bleiben ueber GS-Aufrufe (Vertices/Draws) erhalten. */
static float g_gs_outs[2][16][4];
static int g_gs_bank;
static float g_vs_def_attr[16][4];   /* input_default_attributes (0x232) */

static void p_shader_reset(PicaShader *st)
{
    memset(st, 0, sizeof(*st));
}

static void p_vs_reset(void)
{
    memset(g_vs_def_attr, 0, sizeof(g_vs_def_attr));
    memset(g_gs_outs, 0, sizeof(g_gs_outs));
    g_gs_bank = 0;
    p_shader_reset(&g_vs);
    p_shader_reset(&g_gs);
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

/* Generisches Pica::Float<M,E>::FromRaw (Azahar pica_types.h) fuer f16
   (10/5, Lichtposition) und f20 (12/7, Distanzdaempfung). */
static float p_pica_small_float(uint32_t hex, int mbits, int ebits)
{
    int bias = 127 - ((1 << (ebits - 1)) - 1);
    uint32_t exp_mask = (1u << ebits) - 1u;
    uint32_t mant_mask = (1u << mbits) - 1u;
    uint32_t exponent = (hex >> mbits) & exp_mask;
    uint32_t mantissa = hex & mant_mask;
    uint32_t sign = (hex >> (ebits + mbits)) << 31;
    uint32_t fp32;

    if (exponent == 0u && mantissa == 0u) {
        fp32 = sign;
    } else if (exponent == 0u) {
        int e = bias + 1;
        while ((mantissa & (1u << mbits)) == 0u) {
            e--;
            mantissa <<= 1;
        }
        mantissa &= mant_mask;
        fp32 = sign | ((uint32_t)e << 23) | (mantissa << (23 - mbits));
    } else if (exponent == exp_mask) {
        fp32 = sign | (0xFFu << 23) | (mantissa << (23 - mbits));
    } else {
        fp32 = sign | ((uint32_t)(bias + (int)exponent) << 23) |
               (mantissa << (23 - mbits));
    }
    {
        float f;
        memcpy(&f, &fp32, 4);
        return f;
    }
}

/* ================== Prozedurale Texturen (Proctex, Einheit 3) ==========
 * Nachbildung von Azahar renderer_software/sw_proctex.cpp und der LUT-
 * Verwaltung in pica_core.cpp (PicaCore::ProcTex). Register (TexturingRegs):
 *   0x0A8 proctex           clamp u/v (je 3 Bit), color_combiner (4),
 *                           alpha_combiner (4), separate_alpha (Bit14),
 *                           noise_enable (Bit15), u_shift/v_shift (2+2),
 *                           bias_low (Bits20..27, float16, ungenutzt)
 *   0x0A9/0x0AA noise u/v   amplitude fix1.3.12 (Bits0..15, signed),
 *                           phase float16 (Bits16..31)
 *   0x0AB noise_frequency   u/v je float16
 *   0x0AC proctex_lut       filter (Bits0..2), lod_min/max, width (Bits11..18)
 *   0x0AD lut_offset        level0..3 (je 8 Bit; benutzt wird level0)
 *   0x0AF lut_config        index (Bits0..7), ref_table (Bits8..11)
 *   0x0B0..0x0B7 lut_data   jedes Wort landet bei index und inkrementiert
 *                           index (Azahar PicaCore::ProcTex-LUT-Upload)
 * Wertetabellen: 128 Eintraege, Bits0..11 value, Bits12..23 signed diff
 *   (0.0.12). Farbtabellen: 256 Eintraege RGBA8; color_diff je Kanal ein
 *   signed Byte, das als *2 gelesen wird (Farbdifferenz).
 * Der Azahar-Software-Renderer wertet Mipmap-LOD nicht aus ("TODO(wwylele):
 * implement mipmap" in sw_proctex.cpp); die Mipmap-Filter verhalten sich
 * daher wie ihre Nicht-Mipmap-Gegenstuecke (Nearest/Linear).
 */
#define PROCTEX_VALUE_LUT_COUNT 128
#define PROCTEX_COLOR_LUT_COUNT 256

static uint32_t g_proctex_noise[PROCTEX_VALUE_LUT_COUNT];
static uint32_t g_proctex_color_map[PROCTEX_VALUE_LUT_COUNT];
static uint32_t g_proctex_alpha_map[PROCTEX_VALUE_LUT_COUNT];
static uint32_t g_proctex_color[PROCTEX_COLOR_LUT_COUNT];
static uint32_t g_proctex_color_diff[PROCTEX_COLOR_LUT_COUNT];

static void p_proctex_reset(void)
{
    memset(g_proctex_noise, 0, sizeof(g_proctex_noise));
    memset(g_proctex_color_map, 0, sizeof(g_proctex_color_map));
    memset(g_proctex_alpha_map, 0, sizeof(g_proctex_alpha_map));
    memset(g_proctex_color, 0, sizeof(g_proctex_color));
    memset(g_proctex_color_diff, 0, sizeof(g_proctex_color_diff));
}

/* Azahar WriteInternalReg: der Tabellenindex steht in 0xAF, wird nach jedem
 * Datenwort um eins erhoeht (BitField-Assign maskiert auf 8 Bit) und fuer
 * die 128er-Tabellen modulo deren Groesse benutzt. Unbekannte Tabellen
 * inkrementieren den Index ebenfalls. */
static void p_proctex_lut_write(uint32_t value)
{
    uint32_t cfg = p_regs[0xAF];
    uint32_t index = cfg & 0xFFu;
    uint32_t table = (cfg >> 8) & 0xFu;

    switch (table) {
    case 0u: g_proctex_noise[index % PROCTEX_VALUE_LUT_COUNT] = value; break;
    case 2u: g_proctex_color_map[index % PROCTEX_VALUE_LUT_COUNT] = value; break;
    case 3u: g_proctex_alpha_map[index % PROCTEX_VALUE_LUT_COUNT] = value; break;
    case 4u: g_proctex_color[index % PROCTEX_COLOR_LUT_COUNT] = value; break;
    case 5u: g_proctex_color_diff[index % PROCTEX_COLOR_LUT_COUNT] = value; break;
    default: break;
    }
    p_regs[0xAF] = (cfg & ~0xFFu) | ((index + 1u) & 0xFFu);
}

/* sw_proctex.cpp LookupLUT: coord=0 -> lut[0], coord=127/128 -> lut[127],
 * coord=1 -> lut[127] + diff[127]; dazwischen value + frac*diff. */
static float p_proctex_lut_lookup(const uint32_t *lut, float coord)
{
    int index_int;
    int32_t diff;
    float frac;

    coord *= 128.0f;
    index_int = (int)coord;
    if (index_int > PROCTEX_VALUE_LUT_COUNT - 1) index_int = PROCTEX_VALUE_LUT_COUNT - 1;
    if (index_int < 0) index_int = 0;
    frac = coord - (float)index_int;
    diff = (int32_t)((lut[index_int] >> 12) & 0xFFFu);
    if (diff & 0x800) diff -= 0x1000;
    return (float)(lut[index_int] & 0xFFFu) / 4095.0f +
           frac * ((float)diff / 4095.0f);
}

/* sw_proctex.cpp NoiseRand1D/NoiseRand2D (gegen Hardware verifiziert). */
static unsigned p_proctex_rand1d(unsigned v)
{
    static const unsigned table[16] = {0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11};
    return (((v % 9u) + 2u) * 3u & 0xFu) ^ table[(v / 9u) & 0xFu];
}

static float p_proctex_rand2d(unsigned x, unsigned y)
{
    static const unsigned table[16] = {10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14};
    unsigned u2 = p_proctex_rand1d(x);
    unsigned v2 = p_proctex_rand1d(y);

    v2 += ((u2 & 3u) == 1u) ? 4u : 0u;
    v2 ^= (u2 & 1u) * 6u;
    v2 += 10u + u2;
    v2 &= 0xFu;
    v2 ^= table[u2];
    return -1.0f + (float)v2 * 2.0f / 15.0f;
}

/* sw_proctex.cpp NoiseCoef. */
static float p_proctex_noise_coef(float u, float v)
{
    uint32_t nf = p_regs[0xAB];
    uint32_t nu = p_regs[0xA9];
    uint32_t nv = p_regs[0xAA];
    float freq_u = p_pica_small_float(nf & 0xFFFFu, 10, 5);
    float freq_v = p_pica_small_float((nf >> 16) & 0xFFFFu, 10, 5);
    float phase_u = p_pica_small_float((nu >> 16) & 0xFFFFu, 10, 5);
    float phase_v = p_pica_small_float((nv >> 16) & 0xFFFFu, 10, 5);
    float x = 9.0f * freq_u * fabsf(u + phase_u);
    float y = 9.0f * freq_v * fabsf(v + phase_v);
    int x_int = (int)x;
    int y_int = (int)y;
    float x_frac = x - (float)x_int;
    float y_frac = y - (float)y_int;
    float g0 = p_proctex_rand2d((unsigned)x_int, (unsigned)y_int) * (x_frac + y_frac);
    float g1 = p_proctex_rand2d((unsigned)x_int + 1u, (unsigned)y_int) *
               (x_frac + y_frac - 1.0f);
    float g2 = p_proctex_rand2d((unsigned)x_int, (unsigned)y_int + 1u) *
               (x_frac + y_frac - 1.0f);
    float g3 = p_proctex_rand2d((unsigned)x_int + 1u, (unsigned)y_int + 1u) *
               (x_frac + y_frac - 2.0f);
    float x_noise = p_proctex_lut_lookup(g_proctex_noise, x_frac);
    float y_noise = p_proctex_lut_lookup(g_proctex_noise, y_frac);
    /* Common::BilinearInterp: Lerp(Lerp(g0,g1,x), Lerp(g2,g3,x), y). */
    float y0 = g0 * (1.0f - x_noise) + g1 * x_noise;
    float y1 = g2 * (1.0f - x_noise) + g3 * x_noise;
    return y0 * (1.0f - y_noise) + y1 * y_noise;
}

/* sw_proctex.cpp GetShiftOffset. */
static float p_proctex_shift_offset(float v, uint32_t mode, uint32_t clamp_mode)
{
    float offset = (clamp_mode == 3u) ? 1.0f : 0.5f;   /* MirroredRepeat */

    switch (mode) {
    case 0u: return 0.0f;
    case 1u: return offset * (float)(((int)v / 2) % 2);           /* Odd */
    case 2u: return offset * (float)((((int)v + 1) / 2) % 2);     /* Even */
    default: return 0.0f;
    }
}

/* sw_proctex.cpp ClampCoord. */
static float p_proctex_clamp(float coord, uint32_t mode)
{
    switch (mode) {
    case 0u:                                   /* ToZero */
        if (coord > 1.0f) coord = 0.0f;
        return coord;
    case 1u:                                   /* ToEdge */
        return (coord < 1.0f) ? coord : 1.0f;
    case 2u:                                   /* SymmetricalRepeat */
        return coord - floorf(coord);
    case 3u: {                                 /* MirroredRepeat */
        int integer = (int)coord;
        float frac = coord - (float)integer;
        return (integer % 2) == 0 ? frac : (1.0f - frac);
    }
    case 4u:                                   /* Pulse */
        return (coord <= 0.5f) ? 0.0f : 1.0f;
    default:
        return (coord < 1.0f) ? coord : 1.0f;
    }
}

/* sw_proctex.cpp CombineAndMap. */
static float p_proctex_combine(float u, float v, uint32_t combiner,
                               const uint32_t *map_table)
{
    float f;

    switch (combiner) {
    case 0u: f = u; break;
    case 1u: f = u * u; break;
    case 2u: f = v; break;
    case 3u: f = v * v; break;
    case 4u: f = (u + v) * 0.5f; break;                    /* Add */
    case 5u: f = (u * u + v * v) * 0.5f; break;            /* Add2 */
    case 6u: {                                             /* SqrtAdd2 */
        float s = sqrtf(u * u + v * v);
        f = (s < 1.0f) ? s : 1.0f;
        break;
    }
    case 7u: f = (u < v) ? u : v; break;                   /* Min */
    case 8u: f = (u > v) ? u : v; break;                   /* Max */
    case 9u: {                                             /* RMax */
        float s = sqrtf(u * u + v * v);
        float r = ((u + v) * 0.5f + s) * 0.5f;
        f = (r < 1.0f) ? r : 1.0f;
        break;
    }
    default: f = 0.0f; break;
    }
    return p_proctex_lut_lookup(map_table, f);
}

/* sw_proctex.cpp ProcTex: liefert die Farbe von Textureinheit 3. */
static void p_sample_proctex(float u, float v, unsigned char out[4])
{
    uint32_t cfg = p_regs[0xA8];
    uint32_t u_clamp = cfg & 7u;
    uint32_t v_clamp = (cfg >> 3) & 7u;
    uint32_t color_combiner = (cfg >> 6) & 0xFu;
    uint32_t alpha_combiner = (cfg >> 10) & 0xFu;
    uint32_t filter = p_regs[0xAC] & 7u;
    uint32_t width = (p_regs[0xAC] >> 11) & 0xFFu;
    uint32_t offset = p_regs[0xAD] & 0xFFu;
    float u_shift, v_shift, lut_coord, index;
    unsigned char color[4];
    int i;

    u = fabsf(u);
    v = fabsf(v);

    /* Shift-Offset vor der Rauschzugabe (Azahar). */
    u_shift = p_proctex_shift_offset(v, (cfg >> 16) & 3u, u_clamp);
    v_shift = p_proctex_shift_offset(u, (cfg >> 18) & 3u, v_clamp);

    if (cfg & (1u << 15)) {                    /* noise_enable */
        float noise = p_proctex_noise_coef(u, v);
        int32_t amp_u = (int32_t)(p_regs[0xA9] & 0xFFFFu);
        int32_t amp_v = (int32_t)(p_regs[0xAA] & 0xFFFFu);
        if (amp_u & 0x8000) amp_u -= 0x10000;
        if (amp_v & 0x8000) amp_v -= 0x10000;
        u += noise * (float)amp_u / 4095.0f;
        v += noise * (float)amp_v / 4095.0f;
        u = fabsf(u);
        v = fabsf(v);
    }

    u = p_proctex_clamp(u + u_shift, u_clamp);
    v = p_proctex_clamp(v + v_shift, v_clamp);

    lut_coord = p_proctex_combine(u, v, color_combiner, g_proctex_color_map);
    index = (float)offset +
            ((width > 0u) ? lut_coord * (float)(width - 1u) : 0.0f);

    if (filter == 1u || filter == 3u || filter == 5u) {
        int index_int = (int)index;
        float frac = index - (float)index_int;
        if (index_int < 0) index_int = 0;
        if (index_int > PROCTEX_COLOR_LUT_COUNT - 1) index_int = PROCTEX_COLOR_LUT_COUNT - 1;
        for (i = 0; i < 4; i++) {
            int32_t diff = (int32_t)((g_proctex_color_diff[index_int] >> (i * 8)) & 0xFFu);
            float val;
            if (diff & 0x80) diff -= 0x100;
            val = (float)((g_proctex_color[index_int] >> (i * 8)) & 0xFFu) +
                  frac * (float)(diff * 2);
            if (val < 0.0f) val = 0.0f;
            if (val > 255.0f) val = 255.0f;
            color[i] = (unsigned char)val;
        }
    } else {
        int index_int = (int)roundf(index);
        if (index_int < 0) index_int = 0;
        if (index_int > PROCTEX_COLOR_LUT_COUNT - 1) index_int = PROCTEX_COLOR_LUT_COUNT - 1;
        for (i = 0; i < 4; i++)
            color[i] = (unsigned char)((g_proctex_color[index_int] >> (i * 8)) & 0xFFu);
    }

    if (cfg & (1u << 14)) {                    /* separate_alpha */
        float alpha = p_proctex_combine(u, v, alpha_combiner, g_proctex_alpha_map);
        color[3] = (unsigned char)(alpha * 255.0f);
    }

    for (i = 0; i < 4; i++) out[i] = color[i];
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
    /* Physisch: Basis + Loader-Offset ergibt die physische Adresse; erst
       die Summe wird in den Gastspeicher umgerechnet (Basis liegt oft im
       VRAM-Fenster 0x18000000, die Daten per Offset im FCRAM). */
    ac->base = ((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
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
            uint32_t a = gpu_pica_phys_to_guest(ac.base + ac.src[i] + ac.stride[i] * vert);
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

/* Ein Texel einer 8x8-Kachel dekodieren. x,y sind die rohen Koordinaten
 * innerhalb der Kachel; nur die linearen Formate adressieren ueber den
 * Morton-Index, ETC1/ETC1A4 benutzen x/4+2*(y/4) als Subtile
 * (Azahar texture_decode.cpp LookupTexelInTile). */
static void p_decode_texel(const uint8_t *tile, uint32_t fmt, uint32_t x, uint32_t y,
                           unsigned char out[4])
{
    uint32_t mo = p_morton_interleave(x & 7u, y & 7u);
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
        unsigned fx = x & 7u;
        unsigned fy = y & 7u;
        const uint8_t *s = tile + (fx / 4u + 2u * (fy / 4u)) * ssz;
        unsigned char rgb[3];
        if (has_alpha) {
            uint64_t pa = 0;
            unsigned k;
            for (k = 0; k < 8u; k++) pa |= (uint64_t)s[k] << (8u * k);
            out[3] = (unsigned char)p_c4to8((unsigned)((pa >> (4u * ((fx % 4u) * 4u + (fy % 4u)))) & 0xFu));
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

/* Oeffentliche Dekoder fuer den Hardware-Pfad (Textur-Upload). Beide nutzen
   exakt denselben Code wie der Software-Rasterizer, damit alle Formate
   inklusive ETC1/ETC1A4 identisch dekodiert werden. */
uint32_t gpu_pica_tex_tile_size(uint32_t fmt)
{
    return p_tex_tile_size(fmt);
}

void gpu_pica_decode_texel(const uint8_t *tile, uint32_t fmt, uint32_t x,
                           uint32_t y, uint8_t out[4])
{
    p_decode_texel(tile, fmt, x, y, out);
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

/* ---- Decodier-Cache fuer Texturkacheln ----------------------------------
   Der Port legt die 64 Texel einer 8x8-Kachel einmal als RGBA8 ab. Gueltig
   bleibt eine Kachel nur, solange eine Kopie ihrer Quellbytes mit dem
   Gastspielspeicher uebereinstimmt (memcmp). Jede Speicheraenderung - GSP-
   Upload, DisplayTransfer oder CPU-Schreibzugriff - macht sie damit ohne
   Hash-Kollisionsrisiko ungueltig. Zu grosse Texturen und fehlender Speicher
   fallen auf die direkte Dekodierung zurueck. */
#define P_TEXCACHE_MAX (4u * 1024u * 1024u)

typedef struct {
    uint32_t addr, fmt, ts;
    int w, h, tiles;
    unsigned char *rgba;
    unsigned char *ok;
    unsigned char *src;
    int cap_tiles;
    size_t cap_src;
    int last;
    int ready;
    /* 1 = alle Kacheln dieses Draws bereits gegen den Gastspeicher geprueft
       (p_texcache_validate_all); dann liest p_texcache_texel nur noch und
       ist von mehreren Raster-Threads gleichzeitig nutzbar. */
    int checked;
} PTexCache;

static PTexCache g_texcache[3];

/* Puffer fuer tiles Kacheln der Groesse ts bereitstellen (nur vergroessern). */
static int p_texcache_alloc(PTexCache *tc, int tiles, uint32_t ts)
{
    unsigned char *p;
    if (tc->cap_tiles < tiles) {
        p = (unsigned char *)realloc(tc->rgba, (size_t)tiles * 256u);
        if (!p) return 0;
        tc->rgba = p;
        p = (unsigned char *)realloc(tc->ok, (size_t)tiles);
        if (!p) return 0;
        tc->ok = p;
        memset(tc->ok + tc->cap_tiles, 0, (size_t)(tiles - tc->cap_tiles));
        tc->cap_tiles = tiles;
    }
    if (tc->cap_src < (size_t)tiles * ts) {
        p = (unsigned char *)realloc(tc->src, (size_t)tiles * ts);
        if (!p) return 0;
        tc->src = p;
        tc->cap_src = (size_t)tiles * ts;
    }
    return 1;
}

/* Cache fuer (addr,fmt,w,h) bereitstellen; 0 = direkt dekodieren. */
static int p_texcache_prepare(PTexCache *tc, uint32_t addr, uint32_t fmt,
                              int w, int h)
{
    uint32_t ts = p_tex_tile_size(fmt);
    int tiles;

    if (w <= 0 || h <= 0 || ts == 0u || (w & 7) || (h & 7)) return 0;
    tiles = (w / 8) * (h / 8);
    if ((size_t)tiles * 256u + (size_t)tiles * ts + (size_t)tiles > P_TEXCACHE_MAX)
        return 0;
    if (tc->addr == addr && tc->fmt == fmt && tc->w == w && tc->h == h
            && tc->cap_tiles >= tiles && tc->cap_src >= (size_t)tiles * ts)
        return 1;
    if (!p_texcache_alloc(tc, tiles, ts)) return 0;
    /* Nur ein Format-/Kachelgroessenwechsel entwertet die Dekodierung; eine
       andere Adresse wird je Kachel per memcmp geprueft. */
    if (tc->fmt != fmt || tc->ts != ts)
        memset(tc->ok, 0, (size_t)tc->cap_tiles);
    tc->addr = addr;
    tc->fmt = fmt;
    tc->ts = ts;
    tc->w = w;
    tc->h = h;
    tc->tiles = tiles;
    tc->last = -1;
    return 1;
}

/* Ein Texel aus dem Kachel-Cache; 0 = ausserhalb des Gastspielspeichers. */
static int p_texcache_texel(ArmCpu *c, PTexCache *tc, uint32_t base, int x, int y,
                            unsigned char out[4])
{
    int ti = (y / 8) * (tc->w / 8) + (x / 8);
    uint32_t off = base + (uint32_t)ti * tc->ts;
    unsigned char *dst;

    if (off + tc->ts > c->mem_size) return 0;
    /* Nur beim Kachelwechsel gegen die Quelle pruefen; innerhalb einer Kachel
       kann der Gastspielspeicher waehrend eines Draws nicht wechseln, und je
       Draw setzt p_texcache_draw_start() `last` zurueck. */
    if (!tc->checked && ti != tc->last) {
        if (!tc->ok[ti]
                || memcmp(tc->src + (size_t)ti * tc->ts, c->mem + off, tc->ts) != 0) {
            int k;
            memcpy(tc->src + (size_t)ti * tc->ts, c->mem + off, tc->ts);
            dst = tc->rgba + (size_t)ti * 256u;
            for (k = 0; k < 64; k++)
                p_decode_texel(c->mem + off, tc->fmt, (uint32_t)(k & 7),
                               (uint32_t)(k >> 3), dst + (size_t)k * 4u);
            tc->ok[ti] = 1;
        }
        tc->last = ti;
    }
    dst = tc->rgba + (size_t)ti * 256u
        + ((size_t)(y & 7) * 8u + (size_t)(x & 7)) * 4u;
    memcpy(out, dst, 4);
    return 1;
}

/* Je Draw die Kachel-Kurzpruefung neu validieren: der Gastspielspeicher kann
   sich zwischen zwei Draws geaendert haben (Upload, DisplayTransfer, CPU). */
static void p_texcache_draw_start(void)
{
    int i;
    for (i = 0; i < 3; i++) {
        PTexCache *tc = &g_texcache[i];
        uint32_t base_reg = p_tex_base((uint32_t)i);
        uint32_t addr = (p_regs[base_reg + 4u] & 0x0FFFFFFFu) * 8u;
        uint32_t size = p_regs[base_reg + 1u];
        uint32_t fmt = p_regs[p_tex_fmt_reg((uint32_t)i)] & 0xFu;
        int w = (int)((size >> 16) & 0x7FFu);
        int h = (int)(size & 0x7FFu);
        tc->last = -1;
        tc->checked = 0;
        /* Nur Formate mit teurer Dekodierung (ETC1/ETC1A4) cachen; bei 8-Bit-
           Formaten ist die direkte Dekodierung genauso schnell. */
        tc->ready = (fmt == 12u || fmt == 13u)
            && (addr != 0u) && p_texcache_prepare(tc, addr, fmt, w, h);
    }
}

/* Vor parallelem Rasterisieren: alle Kacheln der aktiven Caches einmal
   seriell pruefen/dekodieren (dieselbe Pruefung wie p_texcache_texel, nur
   vorgezogen). Danach schreibt der Texel-Pfad bis zum naechsten Draw nichts
   mehr; das Ergebnis ist identisch zur lazy Dekodierung, weil sich der
   Gastspeicher waehrend eines Draws nicht aendert. */
static void p_texcache_validate_all(ArmCpu *c)
{
    int i, ti;
    for (i = 0; i < 3; i++) {
        PTexCache *tc = &g_texcache[i];
        uint32_t base;
        if (!tc->ready || tc->checked) continue;
        base = gpu_pica_phys_to_guest(tc->addr);
        for (ti = 0; ti < tc->tiles; ti++) {
            uint32_t off = base + (uint32_t)ti * tc->ts;
            if (off + tc->ts > c->mem_size) continue;
            if (!tc->ok[ti]
                    || memcmp(tc->src + (size_t)ti * tc->ts, c->mem + off, tc->ts) != 0) {
                unsigned char *dst = tc->rgba + (size_t)ti * 256u;
                int k;
                memcpy(tc->src + (size_t)ti * tc->ts, c->mem + off, tc->ts);
                for (k = 0; k < 64; k++)
                    p_decode_texel(c->mem + off, tc->fmt, (uint32_t)(k & 7),
                                   (uint32_t)(k >> 3), dst + (size_t)k * 4u);
                tc->ok[ti] = 1;
            }
        }
        tc->checked = 1;
    }
}

/* Texel einer Einheit an (x,y) ohne Wrap (Koordinaten bereits im Bild).
   texaddr ist die physische Basisadresse des Texturbildes (Azahar
   FullTextureConfig/memory.GetPhysicalPointer). */
static int p_lookup_texel(ArmCpu *c, uint32_t unit, uint32_t texaddr, int x, int y,
                          unsigned char out[4])
{
    uint32_t size = p_regs[p_tex_base(unit) + 1u];
    uint32_t fmt = p_regs[p_tex_fmt_reg(unit)] & 0xFu;
    int w = (int)((size >> 16) & 0x7FFu);
    int h = (int)(size & 0x7FFu);
    uint32_t ts = p_tex_tile_size(fmt);
    uint32_t base, off;

    if (w <= 0 || h <= 0 || ts == 0u) return 0;
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    base = gpu_pica_phys_to_guest(texaddr);
    if (unit < 3u) {
        PTexCache *tc = &g_texcache[unit];
        if (tc->ready && tc->addr == texaddr && tc->fmt == fmt)
            return p_texcache_texel(c, tc, base, x, y, out);
    }
    off = base
        + (uint32_t)(y / 8) * ts * (uint32_t)(w / 8)
        + (uint32_t)(x / 8) * ts;
    if (off + ts > c->mem_size) return 0;
    p_decode_texel(c->mem + off, fmt,
                   (uint32_t)(x & 7), (uint32_t)(y & 7), out);
    return 1;
}

/* Eine Adresse inkl. Wrap/Border/Farbtiefe (t invertiert) sampeln. */
static int p_texel_wrapped(ArmCpu *c, uint32_t unit, uint32_t texaddr, int s, int t,
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
    return p_lookup_texel(c, unit, texaddr, s, h - 1 - t, out);
}

/* Physische Basisadresse der Einheit (Azahar GetPhysicalAddress = address*8). */
static uint32_t p_tex_phys_addr(uint32_t unit)
{
    return (p_regs[p_tex_base(unit) + 4u] & 0x0FFFFFFFu) * 8u;
}

/* Cube-Face-Adresse (Azahar TexturingRegs::GetCubePhysicalAddress): Bits
   22..27 der Hauptadresse sind mit allen Zusatzadressen geteilt. */
static uint32_t p_cube_phys_addr(uint32_t face)
{
    uint32_t addr = p_regs[0x85] & 0x0FFFFFFFu;
    if (face != 0u) {
        uint32_t fa = p_regs[0x86u + face - 1u] & 0x3FFFFFu;
        addr = (addr & ~0x3FFFFFu) | fa;
    }
    return addr * 8u;
}

/* Azahar sw_clipper.cpp ConvertCubeCoord: waehlt die dominante Achse, die
   Face-Adresse und liefert u/v im Face-Raum sowie |z| als Shadow-Tiefe. */
static void p_cube_coord(float u, float v, float w, float *ou, float *ov,
                         float *shadow_z, uint32_t *face)
{
    float au = fabsf(u), av = fabsf(v), aw = fabsf(w);
    float x, y, z;

    if (au > av && au > aw) {
        if (u > 0.0f) { *face = 0u; y = -v; } else { *face = 1u; y = v; }
        x = -w; z = u;
    } else if (av > aw) {
        if (v > 0.0f) { *face = 2u; x = u; } else { *face = 3u; x = -u; }
        y = w; z = v;
    } else {
        if (w > 0.0f) { *face = 4u; y = -v; } else { *face = 5u; y = v; }
        x = u; z = w;
    }
    *shadow_z = fabsf(z);
    /* Azahar ConvertCubeCoord teilt ohne Sonderfall durch z (z==0 -> inf/NaN). */
    *ou = x / z * 0.5f + 0.5f;
    *ov = y / z * 0.5f + 0.5f;
}

/* Sample einer Textureinheit 0..2 (Azahar sw_rasterizer.cpp TextureColor).
 * Der Azahar-Software-Renderer wertet die Min-/Mag-Filter nicht aus
 * ("TODO: Apply the min and mag filters"); es wird immer der naechste
 * Texel gelesen. Nur Einheit 0 beachtet den Texturtyp (3dbrew):
 *   1 Cube/4 ShadowCube -> Face-Adresse + Face-Koordinaten (tc0_w als w)
 *   2 Shadow2D          -> optional Perspektivdivision, Shadow-Vergleich
 *   3 Projection2D      -> u/v /= tc0_w
 *   5 Disabled          -> Textur liefert (0,0,0,0)
 */
static void p_sample_tex(ArmCpu *c, uint32_t unit, float u, float v, float tc0_w,
                         unsigned char out[4])
{
    uint32_t base = p_tex_base(unit);
    uint32_t cfg = p_regs[base + 2u];
    uint32_t size = p_regs[base + 1u];
    uint32_t texaddr = p_tex_phys_addr(unit);
    int w = (int)((size >> 16) & 0x7FFu);
    int h = (int)(size & 0x7FFu);
    uint32_t type = 0u;
    float shadow_z = 0.0f;

    out[0] = out[1] = out[2] = out[3] = 0;
    if (!(p_regs[0x80] & (1u << unit))) return;   /* Azahar: {0,0,0,0} */
    if (texaddr == 0u) { out[3] = 255; return; }
    if (w <= 0 || h <= 0) return;

    if (unit == 0u) {
        type = (cfg >> 28) & 7u;
        switch (type) {
        case 1u: case 4u: {                    /* Cube / ShadowCube */
            uint32_t face = 0u;
            float cu, cv;
            p_cube_coord(u, v, tc0_w, &cu, &cv, &shadow_z, &face);
            u = cu; v = cv;
            texaddr = p_cube_phys_addr(face);
            break;
        }
        case 2u:                                /* Shadow2D */
            if (!(p_regs[0x8B] & 1u) && tc0_w != 0.0f) { u /= tc0_w; v /= tc0_w; }
            shadow_z = fabsf(tc0_w);
            break;
        case 3u:                                /* Projection2D */
            if (tc0_w != 0.0f) { u /= tc0_w; v /= tc0_w; }
            break;
        case 5u:                                /* Disabled */
            return;
        default:
            break;
        }
    }

    if (!p_texel_wrapped(c, unit, texaddr, (int)(u * (float)w), (int)(v * (float)h), out))
        out[0] = out[1] = out[2] = out[3] = 0;

    if (unit == 0u && (type == 2u || type == 4u)) {
        /* Azahar: z_int = min(shadow_z,1)*0xFFFFFF - (bias << 1);
           z_ref = (w<<16)|(z<<8)|y; Density = (z_ref >= z_int) ? x : 0. */
        int z_int = (int)((shadow_z < 1.0f ? shadow_z : 1.0f) * 16777215.0f);
        int z_ref;
        unsigned density;
        z_int -= (int)((p_regs[0x8B] >> 1) & 0x7FFFFFu) << 1;
        z_ref = (out[3] << 16) | (out[2] << 8) | out[1];
        density = (z_ref >= z_int) ? out[0] : 0u;
        out[0] = out[1] = out[2] = out[3] = (unsigned char)density;
    }
}

/* ================= TEV (Fragment-Combiner) =================
 * Nachbildung von Azahar renderer_software/sw_rasterizer.cpp WriteTevConfig
 * und sw_texturing.cpp (GetColorModifier/GetAlphaModifier/ColorCombine/
 * AlphaCombine). Register 0x0C0..0x0FD, 6 Stufen a 8 Woerter:
 *   +0 sources, +1 modifiers, +2 ops, +3 const_color, +4 scales.
 */
typedef struct { unsigned char c[4]; } PicaColor;
/* Azahar pica/output_vertex.h: Semantic 0..3 POSITION, 4..7 QUATERNION,
   8..11 COLOR, 12/13 TEXCOORD0, 14/15 TEXCOORD1, 16 TEXCOORD0_W,
   18..20 VIEW, 22/23 TEXCOORD2. */
typedef struct {
    float pos[4];
    float quat[4];
    float color[4];
    float uv[3][2];
    float tc0_w;
    float view[3];
} PicaVertex;

/* Primitive Assembly (Azahar pica/primitive_assembly.cpp): zwei gepufferte
 * Vertices; bei Strip wird jedes zweite Dreieck als (v2,v1,v3) ausgegeben.
 * Der Zustand bleibt ueber Draws erhalten, bis 0x25F (Restart) oder ein
 * Wechsel der Topologie 0x25E ihn zuruecksetzt. In der Topologie Shader
 * invertiert SetWinding() die Windung des naechsten Dreiecks (GS-EMIT mit
 * winding-Bit). */
static PicaVertex g_prim_buf[2];
static int g_prim_index;
static int g_prim_strip_ready;
static int g_prim_winding;
static int g_prim_topology;      /* 0 Liste, 1 Strip, 2 Fan, 3 Shader */

static void p_prim_reset(void)
{
    g_prim_index = 0;
    g_prim_strip_ready = 0;
    g_prim_winding = 0;
}

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


/* ================= Fragment-Lighting (Azahar sw_lighting.cpp) =================
 * Register 0x140..0x1D9 (Pica::LightingRegs):
 *   0x140 + n*0x10  LightSrc n: specular_0, specular_1, diffuse, ambient,
 *                   Position x/y (f16), z (f16), Spot x/y, z (fix1.1.11),
 *                   config, dist_atten_bias/scale (f20)
 *   0x1C0 global_ambient, 0x1C2 max_light_index, 0x1C3 config0,
 *   0x1C4 config1, 0x1C5 lut_config, 0x1C6 disable,
 *   0x1C8..0x1CF lut_data (schreibt LUT[type][index], index++),
 *   0x1D0 abs_lut_input, 0x1D1 lut_input, 0x1D2 lut_scale,
 *   0x1D9 light_enable (3 Bit je Slot).
 */
static void p_light_lut_write(uint32_t value)
{
    uint32_t cfg = p_regs[0x1C5];
    uint32_t type = (cfg >> 8) & 0x1Fu;
    uint32_t idx = cfg & 0xFFu;
    if (type < LIGHT_LUT_COUNT) {
        float diff = (float)((value >> 12) & 0x7FFu) / 2047.0f;
        if (value & (1u << 23)) diff = -diff;
        g_light_lut[type][idx] = value;
        g_light_lut_fval[type][idx] = (float)(value & 0xFFFu) / 4095.0f;
        g_light_lut_fdiff[type][idx] = diff;
    }
    p_regs[0x1C5] = (cfg & ~0xFFu) | ((idx + 1u) & 0xFFu);
}

/* LutEntry: Bits0..11 value (0.0.12), Bits12..22 diff (0.0.11),
   Bit23 neg_difference. */
static float p_light_lut_lookup(uint32_t lut, unsigned index, float delta)
{
    unsigned l = lut % LIGHT_LUT_COUNT;
    unsigned i = index & 0xFFu;
    return g_light_lut_fval[l][i] + g_light_lut_fdiff[l][i] * delta;
}

/* Pica::LightingRegs::LightColor: 10-Bit-Felder, 255 entspricht 1.0. */
static void p_light_color(uint32_t v, float out[3])
{
    out[0] = (float)((v >> 20) & 0x3FFu) / 255.0f;
    out[1] = (float)((v >> 10) & 0x3FFu) / 255.0f;
    out[2] = (float)(v & 0x3FFu) / 255.0f;
}

static int p_light_sampler_supported(uint32_t config, uint32_t sampler)
{
    if (sampler >= 8u && sampler <= 15u) sampler = 8u;   /* SpotlightAttenuation */
    switch (sampler) {
    case 0u: return config != 1u;                        /* Distribution0 */
    case 1u: return config != 0u && config != 1u && config != 5u;
    case 8u: return config != 2u && config != 3u;        /* SpotlightAttenuation */
    case 3u: return config != 0u && config != 2u && config != 4u;  /* Fresnel */
    case 6u: return config != 3u;                        /* ReflectRed */
    case 4u: case 5u:                                    /* ReflectBlue/Green */
        return config == 4u || config == 5u || config == 8u;
    default: return 0;
    }
}

static float p_light_scale(uint32_t s)
{
    switch (s & 7u) {
    case 0u: return 1.0f;
    case 1u: return 2.0f;
    case 2u: return 4.0f;
    case 3u: return 8.0f;
    case 6u: return 0.25f;
    case 7u: return 0.5f;
    default: return 0.0f;
    }
}

static float p_v3_dot(const float a[3], const float b[3])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void p_v3_cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

/* Normalisiert in-place und liefert die Laenge (Azahar Vec3::Normalize). */
static float p_v3_normalize(float v[3])
{
    float len = sqrtf(p_v3_dot(v, v));
    if (len != 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; }
    return len;
}

static void p_v3_normalized(const float in[3], float out[3])
{
    out[0] = in[0]; out[1] = in[1]; out[2] = in[2];
    p_v3_normalize(out);
}

/* Common::QuaternionRotate: v + 2*Cross(q.xyz, Cross(q.xyz, v) + v*q.w). */
static void p_quat_rotate(const float q[4], const float v[3], float o[3])
{
    float qv[3], t[3], c[3];
    int i;
    qv[0] = q[0]; qv[1] = q[1]; qv[2] = q[2];
    p_v3_cross(qv, v, t);
    for (i = 0; i < 3; i++) t[i] += v[i] * q[3];
    p_v3_cross(qv, t, c);
    for (i = 0; i < 3; i++) o[i] = v[i] + 2.0f * c[i];
}

/* Dieselbe Rotation fuer die feste Basis (0,0,1) bzw. (1,0,0) des
 * Default-Zustands (bump_mode None). Bit-identisch zu p_quat_rotate: die
 * Multiplikationen mit exakt 0/1 sind exakt und entfallen hier nur. */
static void p_quat_rotate_z(const float q[4], float o[3])
{
    float c0 = q[1] * q[3] + q[2] * q[0];
    float c1 = q[2] * q[1] - q[0] * q[3];
    float c2 = -(q[0] * q[0]) - q[1] * q[1];
    o[0] = 2.0f * c0;
    o[1] = 2.0f * c1;
    o[2] = 1.0f + 2.0f * c2;
}

static void p_quat_rotate_x(const float q[4], float o[3])
{
    float c0 = -(q[1] * q[1]) - q[2] * q[2];
    float c1 = q[2] * q[3] + q[0] * q[1];
    float c2 = q[0] * q[2] - q[1] * q[3];
    o[0] = 1.0f + 2.0f * c0;
    o[1] = 2.0f * c1;
    o[2] = 2.0f * c2;
}

typedef struct {
    const float *normal;
    const float *half_vector;
    const float *norm_view;
    const float *light_vector;
    const float *tangent;
    float spot_dir[3];
    uint32_t config;
    uint32_t light_cfg;          /* LightSrc::config (Bit1 = two_sided_diffuse) */
} LightCtx;

#define P_LIGHT_LUT_SLOTS 8u

/* Azahar get_lut_value, Rohwert vor Abs/Index/Skalierung. `nh` ist der
 * bereits normalisierte Halbvektor (wird nur bei NH/VH/CP gebraucht). */
static float p_light_lut_raw(const LightCtx *ctx, uint32_t input,
                             const float nh[3])
{
    float proj[3];
    int i;
    switch (input) {
    case 0u:                                  /* NH */
        return p_v3_dot(ctx->normal, nh);
    case 1u:                                  /* VH */
        return p_v3_dot(ctx->norm_view, nh);
    case 2u:                                  /* NV */
        return p_v3_dot(ctx->normal, ctx->norm_view);
    case 3u:                                  /* LN */
        return p_v3_dot(ctx->light_vector, ctx->normal);
    case 4u:                                  /* SP */
        return p_v3_dot(ctx->light_vector, ctx->spot_dir);
    case 5u:                                  /* CP */
        if (ctx->config == 8u) {
            float d = p_v3_dot(ctx->normal, nh);
            for (i = 0; i < 3; i++) proj[i] = nh[i] - ctx->normal[i] * d;
            return p_v3_dot(proj, ctx->tangent);
        }
        return 0.0f;
    default:
        return 0.0f;
    }
}

/* Azahar get_lut_value, Index/Fraktionsteil (Abs-Klemmung + floorf). */
static void p_light_lut_index(const LightCtx *ctx, float result, int abs_val,
                              unsigned *index, float *delta)
{
    if (abs_val) {
        /* Nur bei two_sided_diffuse fabsf, sonst max(result, 0). */
        if (ctx->light_cfg & 2u) {
            result = (result < 0.0f) ? -result : result;
        } else if (result < 0.0f) {
            result = 0.0f;
        }
        {
            float flr = floorf(result * 256.0f);
            if (flr < 0.0f) flr = 0.0f;
            if (flr > 255.0f) flr = 255.0f;
            *index = (unsigned)flr;
            *delta = result * 256.0f - (float)*index;
        }
    } else {
        float flr = floorf(result * 128.0f);
        int8_t signed_index;
        if (flr < -128.0f) flr = -128.0f;
        if (flr > 127.0f) flr = 127.0f;
        signed_index = (int8_t)flr;
        *delta = result * 128.0f - (float)signed_index;
        *index = (unsigned)(uint8_t)signed_index;
    }
}

/* Zwischenspeicher je Licht: mehrere Sampler lesen im Regelfall dieselbe
 * LUT-Eingangs-Id. Rohwert je Id und Index/Delta je (Id, Abs-Modus) werden
 * deshalb nur einmal je Fragment/Licht berechnet; das Ergebnis ist exakt
 * dasselbe wie eine Neuberechnung. */
typedef struct {
    float nh[3];
    int have_nh;
    uint32_t raw_done;
    float raw[P_LIGHT_LUT_SLOTS];
    uint32_t idx_done[2];
    unsigned idx[2][P_LIGHT_LUT_SLOTS];
    float delta[2][P_LIGHT_LUT_SLOTS];
} LightLut;

static void p_light_lut_prepare(LightLut *lc)
{
    lc->have_nh = 0;
    lc->raw_done = 0;
    lc->idx_done[0] = 0;
    lc->idx_done[1] = 0;
}

static float p_light_lut_sample(const LightCtx *ctx, LightLut *lc, uint32_t input,
                                int abs_val, float scale, uint32_t sampler)
{
    unsigned slot = input & (P_LIGHT_LUT_SLOTS - 1u);
    unsigned a = abs_val ? 1u : 0u;

    if (!(lc->raw_done & (1u << slot))) {
        if ((input == 0u || input == 1u || input == 5u) && !lc->have_nh) {
            p_v3_normalized(ctx->half_vector, lc->nh);
            lc->have_nh = 1;
        }
        lc->raw[slot] = p_light_lut_raw(ctx, input, lc->nh);
        lc->raw_done |= 1u << slot;
    }
    if (!(lc->idx_done[a] & (1u << slot))) {
        p_light_lut_index(ctx, lc->raw[slot], abs_val ? 1 : 0,
                          &lc->idx[a][slot], &lc->delta[a][slot]);
        lc->idx_done[a] |= 1u << slot;
    }
    return scale * p_light_lut_lookup(sampler, lc->idx[a][slot], lc->delta[a][slot]);
}

/* ===== Pro Draw konstante Lichtterme =====================================
 * Die Register 0x140..0x1D9 aendern sich waehrend einer Kommandoliste nur
 * zwischen Draws. Alles, was nicht von quat/view/tex abhaengt, wird deshalb
 * beim ersten Fragment nach der letzten Registeraenderung einmal berechnet.
 * Die Werte sind exakt die der frueheren Berechnung je Fragment. */
typedef struct {
    uint32_t num, lcfg;
    float position[3];
    float spot_dir[3];
    float spec0[3], spec1[3], diffuse[3], ambient[3];
    float dist_scale, dist_bias;
    uint32_t dist_lut;
    int directional;               /* lcfg Bit0 = directional */
    int dist_on, spot_on, d0_on, d1_on, fresnel_on;
    int shadow_p_on, shadow_s_on;
    int refl_on[3];
    uint32_t spot_input, d0_input, d1_input, fresnel_input;
    uint32_t refl_input[3];
    int spot_abs, d0_abs, d1_abs, fresnel_abs;
    int refl_abs[3];
    float spot_scale, d0_scale, d1_scale, fresnel_scale;
    float refl_scale[3];
} LightSlot;

typedef struct {
    int valid;
    uint32_t config0, config1, config, max_light, nlights;
    float gal[3];
    LightSlot s[8];
} LightDraw;

static LightDraw g_light_draw;

static void p_light_draw_invalidate(void)
{
    g_light_draw.valid = 0;
}

static float p_light_scale_reg(uint32_t lut_scale, unsigned shift)
{
    return p_light_scale((lut_scale >> shift) & 7u);
}

static const LightDraw *p_light_draw_get(void)
{
    LightDraw *d = &g_light_draw;
    uint32_t li;

    if (d->valid) return d;
    d->config0 = p_regs[0x1C3];
    d->config1 = p_regs[0x1C4];
    d->config = (d->config0 >> 4) & 0xFu;
    d->max_light = p_regs[0x1C2] & 7u;
    d->nlights = d->max_light + 1u;
    p_light_color(p_regs[0x1C0], d->gal);
    {
        const uint32_t lut_input = p_regs[0x1D1];
        const uint32_t lut_scale = p_regs[0x1D2];
        const uint32_t abs_lut = p_regs[0x1D0];
        const uint32_t en = p_regs[0x1D9];
        for (li = 0; li <= d->max_light; li++) {
            LightSlot *s = &d->s[li];
            uint32_t lb;
            s->num = (en >> (li * 4u)) & 7u;
            lb = 0x140u + s->num * 0x10u;
            s->lcfg = p_regs[lb + 9u];
            s->position[0] = p_pica_small_float(p_regs[lb + 4u] & 0xFFFFu, 10, 5);
            s->position[1] = p_pica_small_float((p_regs[lb + 4u] >> 16) & 0xFFFFu, 10, 5);
            s->position[2] = p_pica_small_float(p_regs[lb + 5u] & 0xFFFFu, 10, 5);
            {
                int32_t sx = ((int32_t)(p_regs[lb + 6u] << 19)) >> 19;
                int32_t sy = ((int32_t)((p_regs[lb + 6u] >> 16) << 19)) >> 19;
                int32_t sz = ((int32_t)(p_regs[lb + 7u] << 19)) >> 19;
                s->spot_dir[0] = (float)sx / 2047.0f;
                s->spot_dir[1] = (float)sy / 2047.0f;
                s->spot_dir[2] = (float)sz / 2047.0f;
            }
            s->dist_scale = p_pica_small_float(p_regs[lb + 0xBu] & 0xFFFFFu, 12, 7);
            s->dist_bias = p_pica_small_float(p_regs[lb + 0xAu] & 0xFFFFFu, 12, 7);
            s->dist_lut = 16u + s->num;
            p_light_color(p_regs[lb + 0u], s->spec0);
            p_light_color(p_regs[lb + 1u], s->spec1);
            p_light_color(p_regs[lb + 2u], s->diffuse);
            p_light_color(p_regs[lb + 3u], s->ambient);
            s->directional = (s->lcfg & 1u) != 0u;
            s->dist_on = !(d->config1 & (1u << (24u + s->num)));
            s->shadow_p_on = (d->config0 & (1u << 16))
                             && !(d->config1 & (1u << s->num));
            s->shadow_s_on = (d->config0 & (1u << 17))
                             && !(d->config1 & (1u << s->num));
            s->spot_on = !(d->config1 & (1u << (8u + s->num))) &&
                         p_light_sampler_supported(d->config, 8u);
            s->d0_on = !(d->config1 & (1u << 16)) &&
                       p_light_sampler_supported(d->config, 0u);
            s->d1_on = !(d->config1 & (1u << 17)) &&
                       p_light_sampler_supported(d->config, 1u);
            s->refl_on[0] = !(d->config1 & (1u << 20)) &&
                            p_light_sampler_supported(d->config, 6u);
            s->refl_on[1] = !(d->config1 & (1u << 21)) &&
                            p_light_sampler_supported(d->config, 5u);
            s->refl_on[2] = !(d->config1 & (1u << 22)) &&
                            p_light_sampler_supported(d->config, 4u);
            s->fresnel_on = (li == d->max_light) &&
                            !(d->config1 & (1u << 19)) &&
                            p_light_sampler_supported(d->config, 3u);
            s->spot_input = (lut_input >> 8) & 7u;
            s->d0_input = lut_input & 7u;
            s->d1_input = (lut_input >> 4) & 7u;
            s->fresnel_input = (lut_input >> 12) & 7u;
            s->refl_input[0] = (lut_input >> 24) & 7u;
            s->refl_input[1] = (lut_input >> 20) & 7u;
            s->refl_input[2] = (lut_input >> 16) & 7u;
            s->spot_abs = ((abs_lut >> 9) & 1u) == 0;
            s->d0_abs = ((abs_lut >> 1) & 1u) == 0;
            s->d1_abs = ((abs_lut >> 5) & 1u) == 0;
            s->fresnel_abs = ((abs_lut >> 13) & 1u) == 0;
            s->refl_abs[0] = ((abs_lut >> 25) & 1u) == 0;
            s->refl_abs[1] = ((abs_lut >> 21) & 1u) == 0;
            s->refl_abs[2] = ((abs_lut >> 17) & 1u) == 0;
            s->spot_scale = p_light_scale_reg(lut_scale, 8);
            s->d0_scale = p_light_scale_reg(lut_scale, 0);
            s->d1_scale = p_light_scale_reg(lut_scale, 4);
            s->fresnel_scale = p_light_scale_reg(lut_scale, 12);
            s->refl_scale[0] = p_light_scale_reg(lut_scale, 24);
            s->refl_scale[1] = p_light_scale_reg(lut_scale, 20);
            s->refl_scale[2] = p_light_scale_reg(lut_scale, 16);
        }
    }
    d->valid = 1;
    return d;
}

/* Azahar SwRenderer::ComputeFragmentsColors. */
static void p_compute_fragments(const PicaColor tex[4], const float quat_in[4],
                                const float view_in[3], PicaColor *out_primary,
                                PicaColor *out_secondary)
{
    const LightDraw *ld = p_light_draw_get();
    const uint32_t config0 = ld->config0;
    const uint32_t config1 = ld->config1;
    float shadow[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    /* Wie Azahar: ohne gueltigen Bump-Modus bleiben Normal/Tangente 0. */
    float surface_normal[3] = {0.0f, 0.0f, 0.0f};
    float surface_tangent[3] = {0.0f, 0.0f, 0.0f};
    float normal[3], tangent[3];
    float quat[4], view[3];
    float frag_norm_view[3];
    int default_basis = 0;
    float diffuse_sum[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float specular_sum[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    uint32_t i, li;

    quat[0] = quat_in[0]; quat[1] = quat_in[1];
    quat[2] = quat_in[2]; quat[3] = quat_in[3];
    {
        float l = sqrtf(quat[0] * quat[0] + quat[1] * quat[1] +
                        quat[2] * quat[2] + quat[3] * quat[3]);
        if (l != 0.0f) for (i = 0; i < 4u; i++) quat[i] /= l;
    }
    view[0] = view_in[0]; view[1] = view_in[1]; view[2] = view_in[2];

    if (config0 & 1u) {                       /* enable_shadow */
        uint32_t sel = (config0 >> 24) & 3u;
        for (i = 0; i < 4u; i++) shadow[i] = (float)tex[sel].c[i] / 255.0f;
        if (config0 & (1u << 18))             /* shadow_invert */
            for (i = 0; i < 4u; i++) shadow[i] = 1.0f - shadow[i];
    }

    if (((config0 >> 28) & 3u) != 0u) {       /* bump_mode != None */
        const uint32_t bump_mode = (config0 >> 28) & 3u;
        uint32_t sel = (config0 >> 22) & 3u;
        float perturbation[3];
        for (i = 0; i < 3u; i++)
            perturbation[i] = (float)tex[sel].c[i] / 127.5f - 1.0f;
        default_basis = 0;
        if (bump_mode == 1u) {                /* NormalMap */
            if (!(config0 & (1u << 30))) {    /* disable_bump_renorm */
                float z2 = 1.0f - (perturbation[0] * perturbation[0] +
                                   perturbation[1] * perturbation[1]);
                perturbation[2] = sqrtf(z2 > 0.0f ? z2 : 0.0f);
            }
            surface_normal[0] = perturbation[0];
            surface_normal[1] = perturbation[1];
            surface_normal[2] = perturbation[2];
            surface_tangent[0] = 1.0f;
            surface_tangent[1] = 0.0f;
            surface_tangent[2] = 0.0f;
        } else if (bump_mode == 2u) {         /* TangentMap */
            surface_normal[0] = 0.0f;
            surface_normal[1] = 0.0f;
            surface_normal[2] = 1.0f;
            surface_tangent[0] = perturbation[0];
            surface_tangent[1] = perturbation[1];
            surface_tangent[2] = perturbation[2];
        } else {
            /* bump_mode 3 ist wie bei Azahar unbekannt: Normal und Tangente
               bleiben 0, einmalige Host-Meldung (LOG_ERROR) bei HLE_PICA_LOG. */
            static int bump3_log = -1, bump3_reported = 0;
            if (bump3_log < 0) {
                const char *e = getenv("HLE_PICA_LOG");
                bump3_log = (e && *e) ? 1 : 0;
            }
            if (bump3_log && !bump3_reported) {
                fprintf(stderr, "[gpu] unbekannter bump_mode 3 -> Normal/Tangente 0\n");
                bump3_reported = 1;
            }
        }
    } else {
        surface_normal[0] = 0.0f;
        surface_normal[1] = 0.0f;
        surface_normal[2] = 1.0f;
        surface_tangent[0] = 1.0f;
        surface_tangent[1] = 0.0f;
        surface_tangent[2] = 0.0f;
        default_basis = 1;
    }

    if (default_basis) {
        p_quat_rotate_z(quat, normal);
        p_quat_rotate_x(quat, tangent);
    } else {
        p_quat_rotate(quat, surface_normal, normal);
        p_quat_rotate(quat, surface_tangent, tangent);
    }
    /* Der normierte Sichtvektor ist je Fragment konstant (nur vom
       interpolierten view abhaengig), nicht je Lichtquelle. */
    {
        float nv[3];
        p_v3_normalized(view, nv);
        for (i = 0; i < 3u; i++) frag_norm_view[i] = nv[i];
    }

    for (li = 0; li < ld->nlights; li++) {
        const LightSlot *ls = &ld->s[li];
        const uint32_t lcfg = ls->lcfg;
        float light_vector[3], half_vector[3];
        float refl_value[3], specular_0[3], specular_1[3];
        float shadow_p[3] = {1.0f, 1.0f, 1.0f};
        float shadow_s[3] = {1.0f, 1.0f, 1.0f};
        float dist_atten = 1.0f, spot_atten = 1.0f;
        float d0_lut = 1.0f, d1_lut = 1.0f;
        float dot_product, clamp_highlights = 1.0f;
        float length;
        LightCtx ctx;
        LightLut lut_cache;

        if (ls->directional) {
            light_vector[0] = ls->position[0];
            light_vector[1] = ls->position[1];
            light_vector[2] = ls->position[2];
        } else {
            light_vector[0] = ls->position[0] + view[0];
            light_vector[1] = ls->position[1] + view[1];
            light_vector[2] = ls->position[2] + view[2];
        }
        length = p_v3_normalize(light_vector);
        half_vector[0] = frag_norm_view[0] + light_vector[0];
        half_vector[1] = frag_norm_view[1] + light_vector[1];
        half_vector[2] = frag_norm_view[2] + light_vector[2];

        if (ls->dist_on) {                        /* dist attenuation */
            float sample_loc = ls->dist_scale * length + ls->dist_bias;
            float flr;
            unsigned lutindex;
            float delta;
            if (sample_loc < 0.0f) sample_loc = 0.0f;
            if (sample_loc > 1.0f) sample_loc = 1.0f;
            flr = floorf(sample_loc * 256.0f);
            if (flr < 0.0f) flr = 0.0f;
            if (flr > 255.0f) flr = 255.0f;
            lutindex = (unsigned)flr;
            delta = sample_loc * 256.0f - (float)lutindex;
            dist_atten = p_light_lut_lookup(ls->dist_lut, lutindex, delta);
        }

        ctx.normal = normal;
        ctx.half_vector = half_vector;
        ctx.norm_view = frag_norm_view;
        ctx.light_vector = light_vector;
        ctx.tangent = tangent;
        ctx.config = ld->config;
        ctx.light_cfg = lcfg;
        ctx.spot_dir[0] = ls->spot_dir[0];
        ctx.spot_dir[1] = ls->spot_dir[1];
        ctx.spot_dir[2] = ls->spot_dir[2];
        p_light_lut_prepare(&lut_cache);

        if (ls->spot_on) {
            spot_atten = p_light_lut_sample(&ctx, &lut_cache, ls->spot_input,
                                            ls->spot_abs, ls->spot_scale,
                                            8u + ls->num);
        }

        if (ls->d0_on) {
            d0_lut = p_light_lut_sample(&ctx, &lut_cache, ls->d0_input,
                                        ls->d0_abs, ls->d0_scale, 0u);
        }
        specular_0[0] = d0_lut * ls->spec0[0];
        specular_0[1] = d0_lut * ls->spec0[1];
        specular_0[2] = d0_lut * ls->spec0[2];

        if (ls->refl_on[0]) {
            refl_value[0] = p_light_lut_sample(&ctx, &lut_cache, ls->refl_input[0],
                                               ls->refl_abs[0], ls->refl_scale[0], 6u);
        } else {
            refl_value[0] = 1.0f;
        }
        if (ls->refl_on[1]) {
            refl_value[1] = p_light_lut_sample(&ctx, &lut_cache, ls->refl_input[1],
                                               ls->refl_abs[1], ls->refl_scale[1], 5u);
        } else {
            refl_value[1] = refl_value[0];
        }
        if (ls->refl_on[2]) {
            refl_value[2] = p_light_lut_sample(&ctx, &lut_cache, ls->refl_input[2],
                                               ls->refl_abs[2], ls->refl_scale[2], 4u);
        } else {
            refl_value[2] = refl_value[0];
        }

        if (ls->d1_on) {
            d1_lut = p_light_lut_sample(&ctx, &lut_cache, ls->d1_input,
                                        ls->d1_abs, ls->d1_scale, 1u);
        }
        specular_1[0] = d1_lut * refl_value[0] * ls->spec1[0];
        specular_1[1] = d1_lut * refl_value[1] * ls->spec1[1];
        specular_1[2] = d1_lut * refl_value[2] * ls->spec1[2];

        if (ls->fresnel_on) {
            float fresnel = p_light_lut_sample(&ctx, &lut_cache, ls->fresnel_input,
                                               ls->fresnel_abs, ls->fresnel_scale, 3u);
            if (config0 & (1u << 2)) diffuse_sum[3] = fresnel;
            if (config0 & (1u << 3)) specular_sum[3] = fresnel;
        }

        dot_product = p_v3_dot(light_vector, normal);
        if (lcfg & 2u) {                  /* two_sided_diffuse */
            dot_product = fabsf(dot_product);
        } else if (dot_product < 0.0f) {
            dot_product = 0.0f;
        }

        if (config0 & (1u << 27))         /* clamp_highlights */
            clamp_highlights = (dot_product == 0.0f) ? 0.0f : 1.0f;

        if (lcfg & 0xCu) {                /* geometric_factor_0/1 */
            float geo = p_v3_dot(half_vector, half_vector);
            geo = (geo == 0.0f) ? 0.0f : (dot_product / geo < 1.0f ? dot_product / geo : 1.0f);
            if (lcfg & 4u) {
                specular_0[0] *= geo; specular_0[1] *= geo; specular_0[2] *= geo;
            }
            if (lcfg & 8u) {
                specular_1[0] *= geo; specular_1[1] *= geo; specular_1[2] *= geo;
            }
        }

        if (ls->shadow_p_on) {
            shadow_p[0] = shadow[0]; shadow_p[1] = shadow[1]; shadow_p[2] = shadow[2];
        }
        if (ls->shadow_s_on) {
            shadow_s[0] = shadow[0]; shadow_s[1] = shadow[1]; shadow_s[2] = shadow[2];
        }

        for (i = 0; i < 3u; i++) {
            float diffuse = (ls->diffuse[i] * dot_product * shadow_p[i] +
                             ls->ambient[i]) * dist_atten * spot_atten;
            float specular = (specular_0[i] + specular_1[i]) * clamp_highlights *
                             dist_atten * spot_atten * shadow_s[i];
            diffuse_sum[i] += diffuse;
            specular_sum[i] += specular;
        }
    }

    if (config0 & (1u << 19)) {               /* shadow_alpha */
        if (config0 & (1u << 2)) diffuse_sum[3] *= shadow[3];
        if (config0 & (1u << 3)) specular_sum[3] *= shadow[3];
    }
    for (i = 0; i < 3u; i++) diffuse_sum[i] += ld->gal[i];

    for (i = 0; i < 4u; i++) {
        float d = diffuse_sum[i], s = specular_sum[i];
        if (d < 0.0f) d = 0.0f;
        if (d > 1.0f) d = 1.0f;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;
        out_primary->c[i] = (unsigned char)(d * 255.0f);
        out_secondary->c[i] = (unsigned char)(s * 255.0f);
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

static PicaColor p_tev(const PicaColor tex[4], const PicaColor *primary,
                       const PicaColor *pfc, const PicaColor *sfc)
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

        /* Stufe, die Previous unveraendert durchreicht (Replace ohne Modifier,
           Skalierung 1x, kein Combiner-Buffer-Schreibzugriff), hat keinen
           Effekt. In Stufe 0 ersetzt die Hardware Previous durch Quelle 3,
           dort gilt die Abkuerzung nicht. */
        if (s != 0u && cop == 0u && aop == 0u && cm1 == 0u && am1 == 0u
                && cs1 == 0xFu && as1 == 0xFu
                && (scl & 3u) == 0u && ((scl >> 16) & 3u) == 0u
                && !(buf_in & ((1u << (8u + s)) | (1u << (12u + s))))) {
            continue;
        }

        if (s == 0) {
            if (cs1 == 0xFu) cs1 = cs3;
            if (cs2 == 0xFu) cs2 = cs3;
        }
        src1 = p_tev_source(cs1, tex, primary, pfc, sfc, &buffer, cst, &output);
        src2 = p_tev_source(cs2, tex, primary, pfc, sfc, &buffer, cst, &output);
        src3 = p_tev_source(cs3, tex, primary, pfc, sfc, &buffer, cst, &output);
        p_color_modifier(cm1, &src1, cin[0]);
        p_color_modifier(cm2, &src2, cin[1]);
        p_color_modifier(cm3, &src3, cin[2]);
        p_color_combine(cop, cin, cout);

        if (cop == 7) {         /* Dot3_RGBA: Alpha = Rot */
            aout = cout[0];
        } else {
            asrc1 = p_tev_source(as1, tex, primary, pfc, sfc, &buffer, cst, &output);
            asrc2 = p_tev_source(as2, tex, primary, pfc, sfc, &buffer, cst, &output);
            asrc3 = p_tev_source(as3, tex, primary, pfc, sfc, &buffer, cst, &output);
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
 * Azahar sw_framebuffer.cpp: Adresse 0x11D * 8 physisch, Breite/Hoehe 0x11E
 * Bits 0..10 bzw. 12..21 = tatsaechliche Hoehe - 1,
 * Kachelung 8x8 Morton, Zeilen von unten nach oben. */
static int p_fb_configured(void)
{
    return (p_regs[0x11D] & 0x0FFFFFFFu) != 0u;
}

static int p_fb_bytes_per_px(void)
{
    switch ((p_regs[0x117] >> 16) & 7u) {
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
    uint32_t width = p_regs[0x11E] & 0x7FFu;
    uint32_t hraw = (p_regs[0x11E] >> 12) & 0x3FFu;
    uint32_t bpp = (uint32_t)p_fb_bytes_per_px();
    if (width == 0u) width = (uint32_t)g_fb_w;
    y = hraw - y;                      /* Azahar: framebuffer.height - y */
    return p_morton_offset(x, y, bpp) + (y & ~7u) * width * bpp;
}

static void p_fb_read(ArmCpu *c, uint32_t x, uint32_t y, unsigned char out[4])
{
    uint32_t addr = gpu_pica_phys_to_guest((p_regs[0x11D] & 0x0FFFFFFFu) * 8u);
    uint32_t off = addr + p_fb_pixel_offset(x, y);
    uint32_t fmt = (p_regs[0x117] >> 16) & 7u;
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
    uint32_t addr = gpu_pica_phys_to_guest((p_regs[0x11D] & 0x0FFFFFFFu) * 8u);
    uint32_t off = addr + p_fb_pixel_offset(x, y);
    uint32_t fmt = (p_regs[0x117] >> 16) & 7u;
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

/* ================= Tiefe/Stencil (0x11C, 0x115/0x116) =================
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
    uint32_t width = p_regs[0x11E] & 0x7FFu;
    uint32_t hraw = (p_regs[0x11E] >> 12) & 0x3FFu;
    uint32_t bpp = p_depth_bpp();
    if (width == 0u) width = (uint32_t)g_fb_w;
    y = hraw - y;
    return p_morton_offset(x, y, bpp) + (y & ~7u) * width * bpp;
}

static int p_depth_addr(ArmCpu *c, uint32_t x, uint32_t y, uint32_t *out)
{
    uint32_t reg = p_regs[0x11C] & 0x0FFFFFFFu;
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
    /* Azahar sw_framebuffer.cpp SetDepth: D16 -> EncodeD16 (2 Bytes),
       D24/D24S8 -> EncodeD24/EncodeD24X8 (3 Bytes, Stencil bleibt). */
    c->mem[off] = (uint8_t)(z & 0xFFu);
    c->mem[off + 1u] = (uint8_t)((z >> 8) & 0xFFu);
    if (p_depth_fmt() != 0u)
        c->mem[off + 2u] = (uint8_t)((z >> 16) & 0xFFu);
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


/* Rein lesender Depth-Vergleich fuer den fruehen Abbruch. Es wird nichts
   geschrieben, damit Alpha-Test- und Stencil-Reihenfolge unberuehrt bleiben. */
static int p_depth_prepass_ok(ArmCpu *c, uint32_t x, uint32_t y, float depth)
{
    uint32_t dm = p_regs[0x107];
    uint32_t bits = p_depth_bits();
    uint32_t z = (uint32_t)(depth * (float)((1u << bits) - 1u));
    return p_compare((dm >> 4) & 7u, z, p_depth_get(c, x, y));
}

/* ================= Clipping (Azahar sw_rasterizer.cpp AddTriangle) ================= */
static float p_dot4(const float a[4], const float b[4])
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
}

/* Azahar sw_clipper.cpp FlipQuaternionIfOpposite: entgegengesetzte
   Quaternionen (q und -q beschreiben dieselbe Rotation) vor Clipping und
   Interpolation gleich orientieren. */
static void p_flip_quat_if_opposite(float a[4], const float b[4])
{
    int i;
    if (p_dot4(a, b) >= 0.0f) return;
    for (i = 0; i < 4; i++) a[i] = -a[i];
}

static void p_clip_lerp(float f, const PicaVertex *a, const PicaVertex *b, PicaVertex *o)
{
    int i, k;
    for (i = 0; i < 4; i++) {
        o->pos[i] = a->pos[i] * f + b->pos[i] * (1.0f - f);
        o->quat[i] = a->quat[i] * f + b->quat[i] * (1.0f - f);
        o->color[i] = a->color[i] * f + b->color[i] * (1.0f - f);
    }
    for (k = 0; k < 3; k++)
        for (i = 0; i < 2; i++)
            o->uv[k][i] = a->uv[k][i] * f + b->uv[k][i] * (1.0f - f);
    o->tc0_w = a->tc0_w * f + b->tc0_w * (1.0f - f);
    for (k = 0; k < 3; k++)
        o->view[k] = a->view[k] * f + b->view[k] * (1.0f - f);
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

/* ---- Paralleles Rasterisieren grosser Dreiecke -------------------------
 * Der Software-Rasterizer ist im natuerlichen Boot (Titelbildschirm) der
 * Engpass: HLE_GTIME misst ~70 % der Wandzeit in den PICA-Listen. Die Pixel
 * eines Dreiecks sind disjunkt (jeder Schreibzugriff geht ueber die
 * Framebuffer-, Tiefen- und Stencil-Helfer auf genau die eigene
 * Pixelkoordinate), und waehrend einer Kommandoliste sind p_regs, Texturen
 * und LUTs konstant. Eine zeilenweise Aufteilung auf Host-Threads aendert das
 * Ergebnis daher nicht; kleine Dreiecke bleiben seriell (Synchronaufwand).
 * Der Pool wird nur vom Port aktiviert (gpu_pica_set_raster_threads aus
 * hle_init); die PICA-Selbsttests laufen ebenfalls damit. */
typedef struct {
    ArmCpu *c;
    const PicaVertex *v0, *v1, *v2;
    int x[3], y[3];
    int min_x, max_x;
    int sc_mode, sx1, sy1, sx2, sy2;
    int bias0, bias1, bias2;
    float invw[3];
    /* Je Dreieck einmal gelesener Registerzustand (reine Lesezugriffe). */
    int fb_on, lighting_on, fast_copy, early_z, tc0w_needed;
    int tex_on[3];
    int proctex_on, proctex_ci;
    uint32_t fb_w, fb_h, om, mask;
    int d0, d1, d2;                  /* Kanten-Inkremente je 16er-Schritt in x */
} PicaRows;

#define PICA_RASTER_MAX_WORKERS 4
#define PICA_RASTER_MIN_ROWS 48      /* erst ab ~1/5 Bildhoehe parallel */

typedef struct {
    HANDLE thread;
    HANDLE wake;
    HANDLE done;
    const PicaRows *job;
    int py0, py1;
} PicaRasterWorker;

static PicaRasterWorker g_rw[PICA_RASTER_MAX_WORKERS];
static int g_raster_threads = 0;
static int g_rw_created = 0;
/* Diagnose: wie viele Dreieckszeilen parallel bzw. seriell laufen. */
static unsigned long long g_par_calls, g_ser_calls, g_par_rows, g_ser_rows;
void gpu_pica_par_stats(unsigned long long *pc, unsigned long long *sc,
                        unsigned long long *pr, unsigned long long *sr) {
    if (pc) *pc = g_par_calls;
    if (sc) *sc = g_ser_calls;
    if (pr) *pr = g_par_rows;
    if (sr) *sr = g_ser_rows;
}

static void p_shade_rows(const PicaRows *j, int py_begin, int py_end)
{
    const PicaVertex *v0 = j->v0, *v1 = j->v1, *v2 = j->v2;
    ArmCpu *c = j->c;
    const int *x = j->x, *y = j->y;
    int py, px;
    int e0, e1, e2;
    /* Zaehler lokal fuehren: ein gemeinsames Inkrement je Fragment wuerde
       die Cacheline zwischen den Worker-Threads hin- und herschieben. */
    unsigned long long lfrag = 0, ltex = 0, llit = 0, lfb = 0;

    for (py = py_begin; py < py_end; py += 16) {
        /* Kantenfunktionen je Zeile neu ansetzen und in x inkrementell
           fortschreiben (ganzzahlig exakt, unabhaengig davon, welcher
           Thread die Zeile rechnet). */
        e0 = j->bias0 + p_signed_area(x[1], y[1], x[2], y[2], j->min_x + 8, py);
        e1 = j->bias1 + p_signed_area(x[2], y[2], x[0], y[0], j->min_x + 8, py);
        e2 = j->bias2 + p_signed_area(x[0], y[0], x[1], y[1], j->min_x + 8, py);
        for (px = j->min_x + 8; px < j->max_x;
             px += 16, e0 += j->d0, e1 += j->d1, e2 += j->d2) {
            int w0, w1, w2, wsum;
            float bw0, bw1, bw2, iw, depth, tc0_w;
            float prim[4], tuv[3][2], quat[4], view[3];
            PicaColor primary, tex_col[4];
            PicaColor pfc, sfc;
            PicaColor tev_out;
            unsigned char texbuf[4];
            int i, unit;

            if (j->sc_mode == 1) {      /* Exclude */
                if (px >= j->sx1 && px < j->sx2 && py >= j->sy1 && py < j->sy2) continue;
            }
            /* Keine Schreibzugriffe ausserhalb des konfigurierten
             * Farbziels (Azahar zeichnet nur in den Framebuffer). */
            if (j->fb_on) {
                if ((uint32_t)(px >> 4) >= j->fb_w || (uint32_t)(py >> 4) >= j->fb_h) continue;
            }

            w0 = e0; w1 = e1; w2 = e2;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            wsum = w0 + w1 + w2;
            if (wsum == 0) continue;
            lfrag++;
            bw0 = (float)w0; bw1 = (float)w1; bw2 = (float)w2;
            iw = 1.0f / (j->invw[0] * bw0 + j->invw[1] * bw1 + j->invw[2] * bw2);

            /* Tiefe: Azahar screenpos.z interpoliert, dann Range/Offset
             * (0x4D/0x4E) und optional W-Buffer (0x6D). Der Wert haengt
             * nur von den Baryzentren ab und wird deshalb vor Textur/TEV
             * berechnet; der fruehe Abbruch liest nur. */
            depth = (v0->pos[2] * bw0 + v1->pos[2] * bw1 + v2->pos[2] * bw2) /
                    (float)wsum;
            depth = depth * p_f24_to_float(p_regs[0x4D]) + p_f24_to_float(p_regs[0x4E]);
            if ((p_regs[0x6D] & 1u) == 0u)      /* DepthBuffering::WBuffering */
                depth *= iw * (float)wsum;
            if (depth < 0.0f) depth = 0.0f;
            if (depth > 1.0f) depth = 1.0f;
            if (j->early_z && !p_depth_prepass_ok(c, (uint32_t)(px >> 4),
                                                  (uint32_t)(py >> 4), depth))
                continue;

            for (i = 0; i < 4; i++)
                prim[i] = (v0->color[i] * bw0 + v1->color[i] * bw1 + v2->color[i] * bw2) * iw;

            for (i = 0; i < 3; i++) {
                /* Koordinaten nur interpolieren, wenn die Einheit sampelt
                   oder die prozedurale Textur sie als Quelle waehlt. */
                if (!j->tex_on[i] && !(j->proctex_on && j->proctex_ci == i)) {
                    tuv[i][0] = tuv[i][1] = 0.0f;
                    continue;
                }
                tuv[i][0] = (v0->uv[i][0] * bw0 + v1->uv[i][0] * bw1 +
                             v2->uv[i][0] * bw2) * iw;
                tuv[i][1] = (v0->uv[i][1] * bw0 + v1->uv[i][1] * bw1 +
                             v2->uv[i][1] * bw2) * iw;
            }
            tc0_w = j->tc0w_needed
                ? (v0->tc0_w * bw0 + v1->tc0_w * bw1 + v2->tc0_w * bw2) * iw
                : 0.0f;
            if (j->lighting_on) {
                for (i = 0; i < 4; i++)
                    quat[i] = (v0->quat[i] * bw0 + v1->quat[i] * bw1 +
                               v2->quat[i] * bw2) * iw;
                for (i = 0; i < 3; i++)
                    view[i] = (v0->view[i] * bw0 + v1->view[i] * bw1 +
                               v2->view[i] * bw2) * iw;
            }

            for (i = 0; i < 4; i++) {
                float fv = prim[i];
                if (fv < 0.0f) fv = 0.0f;
                if (fv > 1.0f) fv = 1.0f;
                primary.c[i] = (unsigned char)roundf(fv * 255.0f);
            }
            for (i = 0; i < 4; i++)
                tex_col[i].c[0] = tex_col[i].c[1] = tex_col[i].c[2] = tex_col[i].c[3] = 0;
            for (unit = 0; unit < 3u; unit++) {
                int ci;
                if (!j->tex_on[unit]) continue;   /* deaktivierte Einheit liefert 0 */
                ltex++;
                ci = (unit == 2u && ((p_regs[0x80] >> 13) & 1u)) ? 1 : (int)unit;
                p_sample_tex(c, unit, tuv[ci][0], tuv[ci][1], tc0_w, texbuf);
                for (i = 0; i < 4; i++) tex_col[unit].c[i] = texbuf[i];
            }
            /* Prozedurale Textur (Einheit 3, Azahar sw_rasterizer.cpp
               TextureColor): eigene Koordinatenwahl
               main_config.texture3_coordinates (Bits8..9). */
            if (j->proctex_on) {
                p_sample_proctex(tuv[j->proctex_ci][0], tuv[j->proctex_ci][1], texbuf);
                for (i = 0; i < 4; i++) tex_col[3].c[i] = texbuf[i];
            }

            /* Fragment-Lighting (Azahar sw_rasterizer.cpp): nur wenn
               0x1C6 Bit0 (lighting.disable) = 0. */
            for (i = 0; i < 4; i++)
                pfc.c[i] = sfc.c[i] = 0;
            if (j->lighting_on) {
                llit++;
                p_compute_fragments(tex_col, quat, view, &pfc, &sfc);
            }

            tev_out = p_tev(tex_col, &primary, &pfc, &sfc);

            /* Alpha-Test (Azahar DoAlphaTest) */
            if (p_regs[0x104] & 1u) {
                uint32_t fn = (p_regs[0x104] >> 4) & 7u;
                uint32_t ref = (p_regs[0x104] >> 8) & 0xFFu;
                if (!p_compare(fn, tev_out.c[3], ref)) continue;
            }

            p_apply_fog(depth, &tev_out);
            if (j->fb_on && !p_depth_stencil_test(c, (uint32_t)(px >> 4),
                                                  (uint32_t)(py >> 4), depth))
                continue;

            if (j->fast_copy) {
                /* LogicOp Copy ohne Blending, volle Kanalmaske: das Ziel
                   wird nicht gelesen, Ergebnis ist das TEV-Fragment. */
                lfb++;
                p_fb_write(c, (uint32_t)(px >> 4), (uint32_t)(py >> 4), tev_out.c);
            } else if (j->fb_on) {
                unsigned char dest[4];
                unsigned char result[4];
                unsigned char bl[4];
                uint32_t om = j->om;
                uint32_t mask = j->mask;
                int k;
                p_fb_read(c, (uint32_t)(px >> 4), (uint32_t)(py >> 4), dest);
                lfb++;
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
            }
        }
    }
    g_frag_n += lfrag;
    g_tex_n += ltex;
    g_lit_n += llit;
    g_fb_n += lfb;
}

static DWORD WINAPI p_raster_worker_main(LPVOID arg)
{
    PicaRasterWorker *w = (PicaRasterWorker *)arg;
    for (;;) {
        WaitForSingleObject(w->wake, INFINITE);
        p_shade_rows(w->job, w->py0, w->py1);
        SetEvent(w->done);
    }
    return 0;
}

void gpu_pica_set_raster_threads(int n)
{
    int i;
    if (n < 0) n = 0;
    if (n > PICA_RASTER_MAX_WORKERS) n = PICA_RASTER_MAX_WORKERS;
    g_raster_threads = 0;
    if (n == 0) return;
    if (!g_rw_created) {
        for (i = 0; i < n; i++) {
            PicaRasterWorker *w = &g_rw[i];
            w->wake = CreateEventA(NULL, FALSE, FALSE, NULL);
            w->done = CreateEventA(NULL, FALSE, FALSE, NULL);
            w->thread = NULL;
            if (w->wake != NULL && w->done != NULL)
                w->thread = CreateThread(NULL, 0, p_raster_worker_main, w, 0, NULL);
            if (w->wake == NULL || w->done == NULL || w->thread == NULL) break;
        }
        g_rw_created = 1;
        n = i;
    }
    g_raster_threads = n;
}

/* Verteilt die Pixelzeilen eines Dreiecks auf den Pool (Rest seriell). */
static void p_raster_rows(const PicaRows *j, int py_first, int py_last)
{
    int nw = g_raster_threads;
    int total, seg, idx;

    if (py_last <= py_first) return;
    total = (py_last - py_first + 15) / 16;
    if (nw <= 0 || total < PICA_RASTER_MIN_ROWS) {
        g_ser_calls++;
        g_ser_rows += (unsigned long long)total;
        p_shade_rows(j, py_first, py_last);
        return;
    }
    /* Lazy aufgebaute, gemeinsam genutzte Zustaende vor dem Verteilen
       seriell fertigstellen, damit die Worker nur lesen: Lighting-Tabelle
       je Draw und Textur-Kachelcache. Beides liefert dieselben Werte wie
       der serielle Pfad. */
    if (j->lighting_on) (void)p_light_draw_get();
    p_texcache_validate_all(j->c);
    g_par_calls++;
    g_par_rows += (unsigned long long)total;
    seg = (total + nw) / (nw + 1);
    if (seg < 1) seg = 1;
    for (idx = 0; idx < nw; idx++) {
        int a = idx * seg;
        int b = a + seg;
        PicaRasterWorker *w = &g_rw[idx];
        if (a >= total) break;
        if (b > total) b = total;
        w->job = j;
        w->py0 = py_first + a * 16;
        w->py1 = py_first + b * 16;
        if (w->py1 > py_last) w->py1 = py_last;
        SetEvent(w->wake);
    }
    {
        int a = nw * seg;
        int b = a + seg;
        if (a > total) a = total;
        if (b > total) b = total;
        p_shade_rows(j, py_first + a * 16, py_first + b * 16);
    }
    for (idx = 0; idx < nw; idx++) {
        if (idx * seg >= total) break;
        WaitForSingleObject(g_rw[idx].done, INFINITE);
    }
}

static void p_process_tri(ArmCpu *c, const PicaVertex *v0, const PicaVertex *v1,
                          const PicaVertex *v2, int reversed)
{
    int x[3], y[3];
    int area;
    uint32_t cull = p_regs[0x40] & 3u;

    if (!g_hw_render) hw_sync_guest();

    x[0] = (int)roundf(v0->pos[0] * 16.0f);
    y[0] = (int)roundf(v0->pos[1] * 16.0f);
    x[1] = (int)roundf(v1->pos[0] * 16.0f);
    y[1] = (int)roundf(v1->pos[1] * 16.0f);
    x[2] = (int)roundf(v2->pos[0] * 16.0f);
    y[2] = (int)roundf(v2->pos[1] * 16.0f);
    area = p_signed_area(x[0], y[0], x[1], y[1], x[2], y[2]);

    if (cull == 0u || cull == 3u) {
        if (!reversed && area <= 0) { p_process_tri(c, v0, v2, v1, 1); return; }
    } else {
        if (!reversed && cull == 1u) {   /* KeepClockWise */
            p_process_tri(c, v0, v2, v1, 1);
            return;
        }
        /* Azahar sw_rasterizer.cpp: cull away triangles with SignedArea <= 0. */
        if (area <= 0) return;
    }

    {
        int min_x = x[0] < x[1] ? (x[0] < x[2] ? x[0] : x[2]) : (x[1] < x[2] ? x[1] : x[2]);
        int min_y = y[0] < y[1] ? (y[0] < y[2] ? y[0] : y[2]) : (y[1] < y[2] ? y[1] : y[2]);
        int max_x = x[0] > x[1] ? (x[0] > x[2] ? x[0] : x[2]) : (x[1] > x[2] ? x[1] : x[2]);
        int max_y = y[0] > y[1] ? (y[0] > y[2] ? y[0] : y[2]) : (y[1] > y[2] ? y[1] : y[2]);
        /* Azahar regs_internal.cpp: 0x065 SCISSORTEST_MODE, 0x066 _POS
           (x1/y1), 0x067 _DIM (x2/y2, inklusiv). */
        int sc_mode = (int)(p_regs[0x65] & 3u);
        int sx1 = (int)(p_regs[0x66] & 0x3FFu) << 4;
        int sy1 = (int)((p_regs[0x66] >> 16) & 0x3FFu) << 4;
        int sx2 = (int)((p_regs[0x67] & 0x3FFu) + 1u) << 4;
        int sy2 = (int)(((p_regs[0x67] >> 16) & 0x3FFu) + 1u) << 4;
        PicaRows jr;
        uint32_t dm;
        int tex_type0;
        unsigned u;

        if (sc_mode == 3) {              /* Include */
            if (min_x < sx1) min_x = sx1;
            if (min_y < sy1) min_y = sy1;
            if (max_x > sx2) max_x = sx2;
            if (max_y > sy2) max_y = sy2;
        }
        min_x &= ~0xF; min_y &= ~0xF;
        max_x = (max_x + 0xF) & ~0xF;
        max_y = (max_y + 0xF) & ~0xF;

        jr.c = c;
        jr.v0 = v0; jr.v1 = v1; jr.v2 = v2;
        for (int k = 0; k < 3; k++) { jr.x[k] = x[k]; jr.y[k] = y[k]; }
        jr.min_x = min_x; jr.max_x = max_x;
        jr.sc_mode = sc_mode;
        jr.sx1 = sx1; jr.sy1 = sy1; jr.sx2 = sx2; jr.sy2 = sy2;
        jr.bias0 = p_right_or_flat(x[0], y[0], x[1], y[1], x[2], y[2]) ? -1 : 0;
        jr.bias1 = p_right_or_flat(x[1], y[1], x[2], y[2], x[0], y[0]) ? -1 : 0;
        jr.bias2 = p_right_or_flat(x[2], y[2], x[0], y[0], x[1], y[1]) ? -1 : 0;
        jr.invw[0] = v0->pos[3];
        jr.invw[1] = v1->pos[3];
        jr.invw[2] = v2->pos[3];

        /* Zustand einmal je Dreieck statt je Pixel aus den Registern lesen.
           Die Abfragen sind reine Lesezugriffe und aendern das Ergebnis der
           Rasterisierung nicht. */
        jr.fb_on = p_fb_configured();
        jr.lighting_on = !(p_regs[0x1C6] & 1u);
        jr.fb_w = p_regs[0x11E] & 0x7FFu;
        jr.fb_h = ((p_regs[0x11E] >> 12) & 0x3FFu) + 1u;
        jr.om = p_regs[0x100];
        jr.mask = p_regs[0x107];
        dm = p_regs[0x107];
        jr.fast_copy = !(jr.om & (1u << 8))
            && ((p_regs[0x102] & 0xFu) == 3u)
            && ((jr.mask & 0xF00u) == 0xF00u)
            && ((p_regs[0x113] & 0xFu) != 0u);
        tex_type0 = (int)((p_regs[0x83] >> 28) & 7u);
        for (u = 0; u < 3u; u++)
            jr.tex_on[u] = (int)((p_regs[0x80] >> u) & 1u);
        jr.proctex_on = (int)((p_regs[0x80] >> 10) & 1u);
        jr.proctex_ci = (int)((p_regs[0x80] >> 8) & 3u);
        if (jr.proctex_ci > 2) jr.proctex_ci = 2;    /* Azahar hat nur uv[0..2] */
        /* tc0_w wird nur von den Texturtypen 1..4 (Einheit 0) gelesen. */
        jr.tc0w_needed = jr.tex_on[0] && tex_type0 >= 1 && tex_type0 <= 4;
        /* Frueher, rein lesender Depth-Abbruch vor Textur/TEV. Stencil
           schreibt bei Fehlschlag und bleibt deshalb beim regulaeren Pfad
           (Reihenfolge wie Azahar). */
        jr.early_z = jr.fb_on && (dm & 1u)
            && !((p_regs[0x105] & 1u) && (p_depth_fmt() == 3u));

        /* Kantenfunktionen inkrementell: p_signed_area(x1,y1,x2,y2,px,py)
           aendert sich je 16er-Schritt in x um -16*(y2-y1). Ganzzahlig exakt,
           also identisch zur Neuberechnung. */
        jr.d0 = -(16 * (y[2] - y[1]));
        jr.d1 = -(16 * (y[0] - y[2]));
        jr.d2 = -(16 * (y[1] - y[0]));

        p_raster_rows(&jr, min_y + 8, max_y);
    }
}

/* ---- Hardware-Pfad: Dreiecke nach Clipping an D3D11 (M9) ---- */
static unsigned g_hw_fb_mask;
static void p_hw_fallback(const char *why, unsigned bit) {
    if (g_hw_fb_mask & bit) return;
    g_hw_fb_mask |= bit;
    printf("[hw] Software-Fallback: %s\n", why);
}

static int p_hw_supported(void)
{
    uint32_t u;
    if ((p_regs[0x1C6] & 1u) == 0u) {             /* Fragment-Lighting */
        p_hw_fallback("Fragment-Lighting aktiv", 1u);
        return 0;
    }
    if ((p_regs[0x6D] & 1u) == 0u) {              /* W-Buffer */
        p_hw_fallback("W-Buffer aktiv", 2u);
        return 0;
    }
    if (p_regs[0x80] & (1u << 10)) {              /* prozedurale Textur */
        p_hw_fallback("prozedurale Textur aktiv", 4u);
        return 0;
    }
    /* Texturen: nur adressierte 2D-Bilder (Typ 0) bzw. disabled (5), alle
       Formate mit 8x8-Kacheln. Cube/Shadow/Projection bleiben Software. */
    for (u = 0; u < 3u; u++) {
        uint32_t base = p_tex_base(u);
        uint32_t cfg = p_regs[base + 2u];
        uint32_t size = p_regs[base + 1u];
        uint32_t fmt = p_regs[p_tex_fmt_reg(u)] & 0xFu;
        uint32_t type = (u == 0u) ? ((cfg >> 28) & 7u) : 0u;
        int w = (int)((size >> 16) & 0x7FFu);
        int h = (int)(size & 0x7FFu);
        if (!(p_regs[0x80] & (1u << u))) continue;
        if (type != 0u) { p_hw_fallback("Texturtyp != Texture2D", 8u); return 0; }
        if (fmt > 13u) { p_hw_fallback("Texturformat > 13", 16u); return 0; }
        if (w <= 0 || h <= 0 || (w & 7) || (h & 7)) {
            p_hw_fallback("Texturgroesse nicht 8x8-vielfach", 32u);
            return 0;
        }
        if (gpu_pica_tex_tile_size(fmt) == 0u) {
            p_hw_fallback("Texturformat ohne Kachelgroesse", 64u);
            return 0;
        }
    }
    return 1;
}

static void p_hw_state(HwDrawState *st, ArmCpu *c)
{
    uint32_t fb = (p_regs[0x11D] & 0x0FFFFFFFu) * 8u;
    uint32_t dp = (p_regs[0x11C] & 0x0FFFFFFFu) * 8u;
    uint32_t dm = p_regs[0x107];
    uint32_t fb_guest = gpu_pica_phys_to_guest(fb);
    uint32_t dp_guest = gpu_pica_phys_to_guest(dp);

    memset(st, 0, sizeof(*st));
    st->color_mem = c->mem + fb_guest;
    st->color_size = c->mem_size > fb_guest ? c->mem_size - fb_guest : 0u;
    st->color_width = p_regs[0x11E] & 0x7FFu;
    st->color_height = ((p_regs[0x11E] >> 12) & 0x3FFu) + 1u;
    st->color_format = (p_regs[0x117] >> 16) & 7u;
    if (dp != 0u) {
        st->depth_mem = c->mem + dp_guest;
        st->depth_size = c->mem_size > dp_guest ? c->mem_size - dp_guest : 0u;
    }
    st->depth_format = p_depth_fmt();
    st->depth_test = (int)(dm & 1u);
    st->depth_write = (int)(((dm >> 12) & 1u) && ((p_regs[0x115] & 3u) != 0u));
    st->depth_func = (dm >> 4) & 7u;
    st->viewport_half_w = p_f24_to_float(p_regs[0x41]);
    st->viewport_half_h = p_f24_to_float(p_regs[0x43]);
    st->viewport_corner_x = (int)(p_regs[0x68] << 22) >> 22;
    st->viewport_corner_y = (int)(p_regs[0x68] << 6) >> 22;
    st->depth_range = p_f24_to_float(p_regs[0x4D]);
    st->depth_near = p_f24_to_float(p_regs[0x4E]);
    st->scissor_mode = p_regs[0x65] & 3u;
    st->scissor_x1 = (int)(p_regs[0x66] & 0x3FFu);
    st->scissor_y1 = (int)((p_regs[0x66] >> 16) & 0x3FFu);
    st->scissor_x2 = (int)((p_regs[0x67] & 0x3FFu) + 1u);
    st->scissor_y2 = (int)(((p_regs[0x67] >> 16) & 0x3FFu) + 1u);
    st->cull = p_regs[0x40] & 3u;
    st->alpha_test = (int)(p_regs[0x104] & 1u);
    st->alpha_func = (p_regs[0x104] >> 4) & 7u;
    st->alpha_ref = (p_regs[0x104] >> 8) & 0xFFu;

    /* Output-Merger 0x100..0x107/0x113. */
    st->blend_enable = (int)((p_regs[0x100] >> 8) & 1u);
    st->blend_eq_rgb = p_regs[0x101] & 7u;
    st->blend_eq_a = (p_regs[0x101] >> 8) & 7u;
    st->blend_src_rgb = (p_regs[0x101] >> 16) & 0xFu;
    st->blend_dst_rgb = (p_regs[0x101] >> 20) & 0xFu;
    st->blend_src_a = (p_regs[0x101] >> 24) & 0xFu;
    st->blend_dst_a = (p_regs[0x101] >> 28) & 0xFu;
    st->blend_const = p_regs[0x103];
    st->logic_op = p_regs[0x102] & 0xFu;
    st->color_write_mask = (p_regs[0x107] >> 8) & 0xFu;
    st->color_write_enable = (p_regs[0x113] & 0xFu) != 0u;

    /* Stencil 0x105/0x106 (nur bei D24S8 wirksam, wie sw_rasterizer.cpp). */
    st->stencil_enable = (int)((p_regs[0x105] & 1u) && p_depth_fmt() == 3u);
    st->stencil_func = (p_regs[0x105] >> 4) & 7u;
    st->stencil_input_mask = (p_regs[0x105] >> 24) & 0xFFu;
    st->stencil_ref = (p_regs[0x105] >> 16) & 0xFFu;
    st->stencil_write_mask = ((p_regs[0x115] & 3u) != 0u)
                                 ? ((p_regs[0x105] >> 8) & 0xFFu) : 0u;
    st->stencil_fail_op = p_regs[0x106] & 7u;
    st->stencil_zfail_op = (p_regs[0x106] >> 4) & 7u;
    st->stencil_pass_op = (p_regs[0x106] >> 8) & 7u;

    /* Fog 0xE0/0xE1; LUT-Schnappschuss aus den per 0xE8..0xEF geladenen
       Eintraegen (p_fog_lut_write). */
    st->fog_mode = (int)(p_regs[0xE0] & 7u);
    st->fog_flip = (int)((p_regs[0xE0] >> 16) & 1u);
    st->fog_color = p_regs[0xE1];
    memcpy(st->fog_lut, g_fog_lut, sizeof(st->fog_lut));

    {
        int i;
        for (i = 0; i < 6; i++) {
            uint32_t b = TEV_STAGE_BASE[i];
            st->tev_src[i] = p_regs[b + 0u];
            st->tev_mod[i] = p_regs[b + 1u];
            st->tev_ops[i] = p_regs[b + 2u];
            st->tev_cst[i] = p_regs[b + 3u];
            st->tev_scl[i] = p_regs[b + 4u];
        }
    }
    st->tev_buf_in = p_regs[0xE0];
    st->tev_buf_color = p_regs[0xFD];

    /* Texturzustand fuer den D3D11-Cache (Adresse schon Gast-linear). */
    st->guest_mem = c->mem;
    st->guest_size = (uint32_t)c->mem_size;
    st->tex_generation = gpu_pica_tex_generation();
    {
        uint32_t u;
        for (u = 0; u < 3u; u++) {
            uint32_t base = p_tex_base(u);
            uint32_t cfg = p_regs[base + 2u];
            uint32_t size = p_regs[base + 1u];
            uint32_t fmt = p_regs[p_tex_fmt_reg(u)] & 0xFu;
            uint32_t addr = (p_regs[base + 4u] & 0x0FFFFFFFu) * 8u;
            int w = (int)((size >> 16) & 0x7FFu);
            int h = (int)(size & 0x7FFu);
            uint32_t ts = gpu_pica_tex_tile_size(fmt);
            HwTexture *t = &st->tex[u];

            t->enabled = (p_regs[0x80] & (1u << u)) ? 1 : 0;
            t->format = fmt;
            t->width = (uint32_t)(w > 0 ? w : 0);
            t->height = (uint32_t)(h > 0 ? h : 0);
            t->wrap_s = (cfg >> 12) & 7u;
            t->wrap_t = (cfg >> 8) & 7u;
            t->border_rgba = p_regs[base];
            /* Azahar sw_rasterizer.cpp ("TODO: Apply the min and mag
               filters"): der Software-Renderer filtert nie, es wird immer
               der naechste Texel genommen. Der Hardware-Pfad muss fuer den
               Bildvergleich dieselbe Referenz-Semantik nutzen. */
            t->filter = 0u;
            t->type = (u == 0u) ? ((cfg >> 28) & 7u) : 0u;
            t->offset = gpu_pica_phys_to_guest(addr);
            t->bytes = 0u;
            if (t->enabled && addr != 0u && w >= 8 && h >= 8 && ts != 0u) {
                uint64_t n = (uint64_t)(w / 8) * (uint64_t)(h / 8) * ts;
                if (n <= 64ull * 1024ull * 1024ull)
                    t->bytes = (uint32_t)n;
            }
        }
    }

    /* Diagnose (HLE_HW_STATE): jede neue Kombination der Output-Merger-/
       Stencil-/Fog-Register einmal melden. Reines Lesen. */
    {
        static int on = -1;
        static uint32_t seen[96];
        static int nseen;
        if (on < 0) { const char *e = getenv("HLE_HW_STATE"); on = (e && *e) ? atoi(e) : 0; }
        if (on) {
            uint32_t key = p_regs[0x100] * 2654435761u ^ p_regs[0x101]
                         ^ (p_regs[0x102] << 3) ^ (p_regs[0x103] << 7)
                         ^ (p_regs[0x107] << 11) ^ (p_regs[0x113] << 15)
                         ^ (p_regs[0x105] << 17) ^ (p_regs[0x106] << 21)
                         ^ (p_regs[0xE0] << 23) ^ (p_regs[0xE1] << 27);
            int i, known = 0;
            for (i = 0; i < nseen; i++) if (seen[i] == key) { known = 1; break; }
            if (!known && nseen < 96) {
                seen[nseen++] = key;
                printf("[hwstate] om=%08X bl=%08X lop=%X mask=%03X cw=%X "
                       "sten=%04X/%X dfmt=%X depthmask=%04X fog=%08X/%08X "
                       "tgt=%08X %ux%u fmt=%u\n",
                       p_regs[0x100], p_regs[0x101], p_regs[0x102] & 0xFu,
                       p_regs[0x107] & 0xFFFu, p_regs[0x113] & 0xFu,
                       p_regs[0x105], p_regs[0x106], p_regs[0x116] & 3u,
                       p_regs[0x107], p_regs[0xE0], p_regs[0xE1],
                       (p_regs[0x11D] & 0x0FFFFFFFu) * 8u,
                       p_regs[0x11E] & 0x7FFu,
                       ((p_regs[0x11E] >> 12) & 0x3FFu) + 1u,
                       (p_regs[0x117] >> 16) & 7u);
                printf("[hwstate] texen=%08X t0=%08X/%08X f=%X t1=%08X/%08X f=%X "
                       "tev0 src=%08X op=%08X scl=%X tev1 src=%08X op=%08X\n",
                       p_regs[0x80], p_regs[0x81], p_regs[0x82], p_regs[0x8E] & 0xFu,
                       p_regs[0x91], p_regs[0x92], p_regs[0x96] & 0xFu,
                       p_regs[0xC0], p_regs[0xC2], p_regs[0xC4] & 0xFu,
                       p_regs[0xC8], p_regs[0xCA]);
                fflush(stdout);
            }
        }
    }
}

/* Rueckgabe 1 = vom Hardware-Pfad uebernommen, 0 = Software faellt ein. */
static int p_hw_tri_clip(ArmCpu *c, const PicaVertex *in, int n)
{
    HwDrawState st;
    HwVertex out[16 * 4];
    float half_x, half_y;
    int corner_x, corner_y, i, m = 0;

    if (!p_hw_supported()) {
        hw_sync_guest();
        return 0;
    }
    p_hw_state(&st, c);
    if (!hw_begin_draw(&st)) return 0;

    half_x = p_f24_to_float(p_regs[0x41]);
    half_y = p_f24_to_float(p_regs[0x43]);
    corner_x = (int)(p_regs[0x68] << 22) >> 22;
    corner_y = (int)(p_regs[0x68] << 6) >> 22;
    for (i = 0; i + 2 < n && m + 3 <= (int)(sizeof(out) / sizeof(out[0])); i++) {
        const PicaVertex *t[3] = { &in[0], &in[i + 1], &in[i + 2] };
        int x[3], y[3];
        int k, area, keep = 1;
        for (k = 0; k < 3; k++) {
            float invw = 1.0f / t[k]->pos[3];
            float sx = (t[k]->pos[0] * invw + 1.0f) * half_x + (float)corner_x;
            float sy = (t[k]->pos[1] * invw + 1.0f) * half_y + (float)corner_y;
            x[k] = (int)roundf(sx * 16.0f);
            y[k] = (int)roundf(sy * 16.0f);
        }
        area = p_signed_area(x[0], y[0], x[1], y[1], x[2], y[2]);
        if (st.cull == 1u) keep = area < 0;
        else if (st.cull == 2u) keep = area > 0;
        if (!keep) continue;
        for (k = 0; k < 3; k++) {
            memcpy(out[m].pos, t[k]->pos, 4 * sizeof(float));
            memcpy(out[m].color, t[k]->color, 4 * sizeof(float));
            memcpy(out[m].uv, t[k]->uv, sizeof(out[m].uv));
            if (p_regs[0x80] & (1u << 13)) {   /* texture2_use_coord1 */
                out[m].uv[2][0] = t[k]->uv[1][0];
                out[m].uv[2][1] = t[k]->uv[1][1];
            }
            m++;
        }
    }
    if (m > 0) hw_triangles(out, m);
    return 1;
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
    /* Azahar AddTriangle (vor dem Sutherland-Hodgman-Clipping). */
    p_flip_quat_if_opposite(buf_a[1].quat, buf_a[0].quat);
    p_flip_quat_if_opposite(buf_a[2].quat, buf_a[0].quat);

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

    if (g_hw_render) {
        if (p_hw_tri_clip(c, in, n)) return;
        /* Software-Fallback im HW-Modus: dieses Dreieck schreibt gleich in
           den Gastpuffer (evtl. eine Textur). Der D3D11-Cache muss gebundene
           Texturen beim naechsten HW-Draw neu pruefen. Im reinen SW-Modus
           erhoeht das Draw-Ende die Generation (kein Zusatzaufwand). */
        gpu_pica_bump_tex_generation();
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
        /* Azahar sw_rasterizer.cpp MakeScreenCoords: quat, color, tc0,
           tc1, tc0_w, view und tc2 werden mit 1/w multipliziert; die
           Interpolation teilt durch die interpolierte 1/w-Summe
           (perspektivkorrekt). Ohne das waeren View (Punktlicht-Vektor,
           Distanzdaempfung) und Quaternion (Normale) affin bzw. um w
           skaliert interpoliert. */
        for (k = 0; k < 4; k++) in[i].quat[k] *= invw;
        for (k = 0; k < 4; k++) in[i].color[k] *= invw;
        for (m = 0; m < 3; m++)
            for (k = 0; k < 2; k++) in[i].uv[m][k] *= invw;
        in[i].tc0_w *= invw;
        for (k = 0; k < 3; k++) in[i].view[k] *= invw;
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
 *           (invertiert: src1i 14..18, src2i 7..13; gilt fuer
 *            0x18..0x1B DPHI/DSTI/SGEI/SLTI und 0x30..0x37 MADI)
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

static const float *p_vs_src(const VsUnit *u, const PicaShader *st,
                             uint32_t reg, int ari)
{
    if (ari) {
        int32_t off = u->addr[ari - 1];
        if (off < -128 || off > 127) off = 0;
        if (reg >= 0x20u && reg < 0x20u + 96u) {
            uint32_t idx = (uint32_t)((int32_t)(reg - 0x20u) + off) & 0x7Fu;
            if (idx >= 96u) return g_vs_ones;
            return st->f[idx];
        }
    }
    if (reg < 0x10u) return u->in[reg];
    if (reg < 0x20u) return u->tmp[reg - 0x10u];
    if (reg < 0x20u + 96u) return st->f[reg - 0x20u];
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

/* ---- Geometrie-Emitter (Azahar shader_unit.cpp GeometryEmitter) ----
 * EMIT kopiert die Ausgangsregister (0x28D output_mask) nach
 * buffer[vertex_id]; bei prim_emit gehen anschliessend alle drei Puffer in
 * die Primitive Assembly. SETEMIT setzt vertex_id/prim_emit/winding
 * (nihstro setemit: Bits0..1 vertex_id, Bit2 prim_emit, Bit3 winding). */
typedef struct {
    float buffer[3][16][4];
    int vertex_id;
    int prim_emit;
    int winding;
} PicaEmitter;

static PicaEmitter g_emit;

/* Azahar GeometryShaderUnit::ConfigOutput kopiert 0x28D bei Reconfigure in den
 * Emitter; spaetere Register-Schreibzugriffe wirken erst beim naechsten Draw. */
static unsigned g_gs_emit_mask;

static void p_out_vertex(const float outs[16][4], PicaVertex *v);
static void p_prim_submit(ArmCpu *c, const PicaVertex *v);

static void p_gs_emit(ArmCpu *c, const float outs[16][4])
{
    uint32_t mask = g_gs_emit_mask & 0xFFFFu;   /* bei Reconfigure kopiert */
    uint32_t reg, oi = 0;
    int i;

    if (g_emit.vertex_id < 0 || g_emit.vertex_id > 2) return;
    for (reg = 0; reg < 16u; reg++) {
        if (!(mask & (1u << reg))) continue;
        memcpy(g_emit.buffer[g_emit.vertex_id][oi], outs[reg], 4 * sizeof(float));
        oi++;
    }
    if (!g_emit.prim_emit) return;
    if (g_emit.winding) g_prim_winding = 1;
    for (i = 0; i < 3; i++) {
        PicaVertex v;
        p_out_vertex(g_emit.buffer[i], &v);
        p_prim_submit(c, &v);
    }
}

static void p_vs_execute(ArmCpu *c, const PicaShader *st, VsUnit *u, uint32_t entry)
{
    VsIf ifs[8];
    VsCall calls[4];
    VsLoop loops[4];
    int ifn = 0, ifh = 0, cn = 0, ch = 0, ln = 0, lh = 0;
    uint32_t pc = entry;
    unsigned steps = 0;
    int stop = 0;

    while (!stop && steps++ < VS_MAX_STEPS) {
        uint32_t oldpc = pc;
        uint32_t instr, op;
        int is_break = 0;

        if (pc >= st->code_words) break;
        instr = st->code[pc];
        op = instr >> 26;

        if (op <= 0x1Fu || (op & ~1u) == 0x2Eu) {
            int inv = (op >= 0x18u && op <= 0x1Bu);
            uint32_t desc = instr & 0x7Fu;
            uint32_t s1r = inv ? ((instr >> 14) & 0x1Fu) : ((instr >> 12) & 0x7Fu);
            uint32_t s2r = inv ? ((instr >> 7) & 0x7Fu) : ((instr >> 7) & 0x1Fu);
            uint32_t ari = (instr >> 19) & 3u;
            uint32_t dreg = (instr >> 21) & 0x1Fu;
            uint32_t sw = st->swz[desc & (VS_SWZ_WORDS - 1u)];
            const float *r1 = p_vs_src(u, st, s1r, inv ? 0 : (int)ari);
            const float *r2 = p_vs_src(u, st, s2r, inv ? (int)ari : 0);
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
            case 0x03u:
            case 0x18u: {              /* DP3 / DP4 / DPH / DPHI */
                int nc = (op == 0x01u) ? 3 : 4;
                float dot = 0.0f;
                if (op == 0x03u || op == 0x18u) s1[3] = 1.0f;
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
            uint32_t sw = st->swz[desc & (VS_SWZ_WORDS - 1u)];
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
            r1 = p_vs_src(u, st, s1r, 0);
            r2 = p_vs_src(u, st, s2r, inv ? 0 : (int)ari);
            r3 = p_vs_src(u, st, s3r, inv ? (int)ari : 0);
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
                cond = ((st->b[bid] != 0) == ((num & 1u) == 0u));
                if (cond) pc = dest - 1;
                break;
            case 0x24u:                /* CALL */
            case 0x25u:                /* CALLC */
            case 0x26u: {              /* CALLU */
                int take = 1;
                VsCall v;
                if (op == 0x26u) take = (st->b[bid] != 0);
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
                cond = (op == 0x27u) ? (st->b[bid] != 0)
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
                u->addr[2] = (int32_t)st->i[iid][1];
                v.entry = pc + 1;
                v.end = dest + 1;
                v.down = st->i[iid][0];
                v.inc = st->i[iid][2];
                v.prev = (unsigned)st->i[iid][1];
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
            case 0x2Au:                /* EMIT (nur Geometrie-Einheit) */
                if (st == &g_gs) {
                    p_gs_emit(c, u->out[u->bank]);
                    u->bank = !u->bank;
                }
                break;
            case 0x2Bu:                /* SETEMIT (nur Geometrie-Einheit) */
                if (st == &g_gs) {
                    g_emit.vertex_id = (int)(instr & 3u);
                    g_emit.prim_emit = (int)((instr >> 2) & 1u);
                    g_emit.winding = (int)((instr >> 3) & 1u);
                }
                break;
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

/* ---- Ein-/Ausgangs-Mapping (Azahar shader_unit.cpp LoadInput/WriteOutput) ----
 * Fuehrt eine Shader-Einheit aus: Attribute laut Input-Map in die
 * Eingangsregister, Programm ab `entry`, Ausgangsregister laut `outmask`
 * kompakt nach outs[0..]. Die Semantik-Zuordnung (0x04F/0x050) passiert
 * erst danach in p_out_vertex. */
static void p_shader_run(ArmCpu *c, const PicaShader *st, uint32_t entry,
                         const float attrs[16][4], uint32_t maxa,
                         uint64_t map, uint32_t outmask, float outs[16][4])
{
    VsUnit u;
    uint32_t a, oi = 0;

    memset(&u, 0, sizeof(u));
    if (st == &g_gs) {
        /* Ausgangsbank und -register der Einheit 3 ueber Aufrufe erhalten. */
        memcpy(u.out, g_gs_outs, sizeof(u.out));
        u.bank = g_gs_bank;
    }
    for (a = 0; a <= maxa && a < 16u; a++) {
        uint32_t reg = (uint32_t)((map >> (a * 4u)) & 0xFu);
        if (reg < 16u) memcpy(u.in[reg], attrs[a], 4 * sizeof(float));
    }
    p_vs_execute(c, st, &u, entry);
    if (st == &g_gs) {
        memcpy(g_gs_outs, u.out, sizeof(u.out));
        g_gs_bank = u.bank;
    }
    memset(outs, 0, 16 * 4 * sizeof(float));
    for (a = 0; a < 16u; a++) {
        if ((outmask >> a) & 1u) {
            memcpy(outs[oi], u.out[0][a], 4 * sizeof(float));
            oi++;
        }
    }
}

/* Vertex-Shader ausfuehren (Register 0x2B9/0x2BA/0x2BB/0x2BC/0x2BD). */
static void p_vs_process(ArmCpu *c, const float attrs[16][4], float outs[16][4])
{
    uint64_t map = ((uint64_t)p_regs[0x2BC] << 32) | p_regs[0x2BB];
    p_shader_run(c, &g_vs, p_regs[0x2BA] & 0xFFFFu, attrs,
                 p_regs[0x2B9] & 0xFu, map, p_regs[0x2BD] & 0xFFFFu, outs);
}

static void p_out_vertex(const float outs[16][4], PicaVertex *v)
{
    uint32_t a, i;
    float slot[32];

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
    for (i = 0; i < 4; i++) v->quat[i] = slot[4 + i];
    for (i = 0; i < 4; i++) v->color[i] = slot[8 + i];
    v->uv[0][0] = slot[12];
    v->uv[0][1] = slot[13];
    v->uv[1][0] = slot[14];
    v->uv[1][1] = slot[15];
    v->tc0_w = slot[16];           /* Azahar output_vertex.cpp: TEXCOORD0_W */
    v->view[0] = slot[18];         /* VIEW 18..20 */
    v->view[1] = slot[19];
    v->view[2] = slot[20];
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

static void p_emit_tri(ArmCpu *c, const PicaVertex *a, const PicaVertex *b,
                       const PicaVertex *d)
{
    /* Clip -> Viewport -> Rasterisierung + TEV + Farbziel (Azahar
       sw_rasterizer.cpp). Keine NDC-Heuristik mehr. */
    p_raster_tri(c, a, b, d);
}

/* Vertex in die Primitive Assembly geben (Azahar primitive_assembly.cpp). */
static void p_prim_submit(ArmCpu *c, const PicaVertex *v)
{
    switch (g_prim_topology) {
    case 1: case 2:                                   /* Strip / Fan */
        if (g_prim_strip_ready)
            p_emit_tri(c, &g_prim_buf[0], &g_prim_buf[1], v);
        g_prim_buf[g_prim_index] = *v;
        g_prim_strip_ready |= (g_prim_index == 1);
        if (g_prim_topology == 1)
            g_prim_index = !g_prim_index;
        else
            g_prim_index = 1;
        break;
    default:                                          /* List / Shader */
        if (g_prim_index < 2) {
            g_prim_buf[g_prim_index++] = *v;
        } else {
            g_prim_index = 0;
            if (g_prim_topology == 3 && g_prim_winding) {
                p_emit_tri(c, &g_prim_buf[1], &g_prim_buf[0], v);
                g_prim_winding = 0;
            } else {
                p_emit_tri(c, &g_prim_buf[0], &g_prim_buf[1], v);
            }
        }
        break;
    }
}

/* ================== Geometrie-Pipeline (Shader-Einheit 3) ==================
 * Azahar pica/geometry_pipeline.cpp. Konfiguration:
 *   0x229 Bits0..1 use_gs (0 No, 2 Yes), Bit31 variable_primitive
 *   0x242 max_input_attrib_index des Vertex-Loaders (Immediate)
 *   0x244 gs_unit_exclusive_configuration: 0 -> VS-Register teilen Programm,
 *         Swizzle, Uniforms, Bool-/Int-Uniforms und output_mask mit Einheit 3
 *         (nur solange use_gs == No)
 *   0x24A/0x251 vs_outmap_total_minus_1_a/b (Anzahl VS-Ausgangsattribute)
 *   0x252 gs_config: Bits0..7 mode (0 Point, 1 VariablePrimitive,
 *         2 FixedPrimitive), 8..11 fixed_vertex_num_minus_1,
 *         12..15 stride_minus_1, 16..23 start_index
 *   0x254 variable_vertex_main_num_minus_1
 *   0x25E triangle_topology (3 = Shader, programmierbare Primitive Assembly)
 *   Point:        VS-Ausgaben fuellen den GS-Eingangspuffer (0x289 Bits0..3);
 *                 ist er voll, laeuft der GS mit Input-Map 0x28B/0x28C.
 *   Variable:     der erste Index eines Batches ist die Vertexzahl (Uniform
 *                 f[0]), die Vertices landen in den Uniforms ab f[1]; nur die
 *                 ersten 0x254+1 Vertices erhalten alle Attribute.
 *   Fixed:        wie Variable, aber feste Vertexzahl ab start_index.
 */
typedef struct {
    int mode;                 /* -1 aus, 0 Point, 1 Variable, 2 Fixed */
    float attr[16][4];        /* Point: GS-Eingangspuffer */
    unsigned cur;             /* naechster Vec4-Slot */
    unsigned end;             /* Point: Puffergroesse in Vec4 */
    int need_index;           /* Variable: naechster Wert ist die Vertexzahl */
    unsigned main_left;       /* Variable: Hauptvertices mit allen Attributen */
    unsigned total_left;      /* Variable: noch erwartete Vertices */
    unsigned fixed_begin;     /* Fixed: erster/letzter Uniform-Slot */
    unsigned fixed_end;
} GsPipeline;

static GsPipeline g_gp = { .mode = -1 };
/* Azahar ImmediateModeState::reset_geometry_pipeline: wird beim Schreiben von
 * 0x232 (Default-Attribut-Index) gesetzt und beim naechsten Immediate-Draw
 * verbraucht. */
static int g_gp_imm_reset = 1;

static unsigned p_gs_vs_outputs(void) { return (p_regs[0x24A] & 0xFu) + 1u; }

/* GS ausfuehren; attrs == NULL bedeutet: Eingaben stehen in den Uniforms
 * (VariablePrimitive/FixedPrimitive, Azahar LoadInput wird nicht aufgerufen). */
static void p_gs_invoke(ArmCpu *c, const float (*attrs)[4])
{
    static const float zero_attrs[16][4];
    float dummy[16][4];
    uint64_t map = ((uint64_t)p_regs[0x28C] << 32) | p_regs[0x28B];

    p_shader_run(c, &g_gs, p_regs[0x28A] & 0xFFFFu,
                 attrs ? attrs : zero_attrs, attrs ? (p_regs[0x289] & 0xFu) : 0u,
                 attrs ? map : 0u, 0u, dummy);
    /* Azahar geometry_pipeline.cpp: b15 ist nach jedem Aufruf true. */
    g_gs.b[15] = 1;
}

/* Azahar GeometryPipeline::Reconfigure (laeuft einmal je Draw). */
static void p_gs_pipeline_reconfigure(void)
{
    unsigned vsn = p_gs_vs_outputs();

    g_gs_emit_mask = p_regs[0x28D] & 0xFFFFu;   /* ConfigOutput(regs.gs) */
    g_gp.mode = -1;
    g_gp.cur = 0;
    g_gp.end = 0;
    g_gp.need_index = 0;
    g_gp.main_left = 0;
    g_gp.total_left = 0;
    if ((p_regs[0x229] & 3u) != 2u) return;      /* PipelineRegs::UseGS::No */

    switch (p_regs[0x252] & 0xFFu) {
    case 0:                                      /* GSMode::Point */
        g_gp.mode = 0;
        g_gp.end = (p_regs[0x289] & 0xFu) + 1u;
        if (g_gp.end > 16u) g_gp.end = 16u;
        break;
    case 1:                                      /* GSMode::VariablePrimitive */
        g_gp.mode = 1;
        g_gp.need_index = 1;
        break;
    case 2:                                      /* GSMode::FixedPrimitive */
        g_gp.fixed_begin = (p_regs[0x252] >> 16) & 0xFFu;
        g_gp.fixed_end = g_gp.fixed_begin +
            vsn * (((p_regs[0x252] >> 8) & 0xFu) + 1u);
        if (g_gp.fixed_begin >= 96u) break;
        if (g_gp.fixed_end > 96u) g_gp.fixed_end = 96u;
        g_gp.mode = 2;
        g_gp.cur = g_gp.fixed_begin;
        break;
    default:
        break;
    }
}

static int p_gs_pipeline_need_index(void)
{
    return g_gp.mode == 1 && g_gp.need_index;
}

/* Azahar GeometryPipeline_VariablePrimitive::SubmitIndex. */
static void p_gs_pipeline_submit_index(unsigned val)
{
    float num[4];

    if (g_gp.mode != 1 || !g_gp.need_index) return;
    num[0] = num[1] = num[2] = num[3] = (float)val;
    memcpy(g_gs.f[0], num, sizeof(num));
    g_gp.cur = 1;
    g_gp.main_left = (p_regs[0x254] & 0xFFu) + 1u;
    g_gp.total_left = val;
    g_gp.need_index = 0;
}

static void p_gs_pipeline_submit(ArmCpu *c, const float vsout[16][4])
{
    unsigned vsn = p_gs_vs_outputs();
    unsigned i;

    switch (g_gp.mode) {
    case 0:                                      /* Point */
        for (i = 0; i < vsn && g_gp.cur < g_gp.end; i++)
            memcpy(g_gp.attr[g_gp.cur++], vsout[i], 4 * sizeof(float));
        if (g_gp.end != 0u && g_gp.cur >= g_gp.end) {
            g_gp.cur = 0;
            p_gs_invoke(c, (const float (*)[4])g_gp.attr);
        }
        break;
    case 1:                                      /* VariablePrimitive */
        if (g_gp.main_left != 0u) {
            for (i = 0; i < vsn && g_gp.cur < 96u; i++)
                memcpy(g_gs.f[g_gp.cur++], vsout[i], 4 * sizeof(float));
            g_gp.main_left--;
        } else if (g_gp.cur < 96u) {
            memcpy(g_gs.f[g_gp.cur++], vsout[0], 4 * sizeof(float));
        }
        if (g_gp.total_left != 0u) g_gp.total_left--;
        if (g_gp.total_left == 0u) {
            g_gp.need_index = 1;
            p_gs_invoke(c, NULL);
        }
        break;
    case 2:                                      /* FixedPrimitive */
        for (i = 0; i < vsn && g_gp.cur < g_gp.fixed_end && g_gp.cur < 96u; i++)
            memcpy(g_gs.f[g_gp.cur++], vsout[i], 4 * sizeof(float));
        if (g_gp.cur >= g_gp.fixed_end) {
            g_gp.cur = g_gp.fixed_begin;
            p_gs_invoke(c, NULL);
        }
        break;
    default:
        break;
    }
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
    /* Indexpuffer: physische Adresse = Attribut-Basis + Offset (0x227),
       erst danach in den Gastspeicher umrechnen. */
    uint32_t base = ((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
    uint32_t off = p_regs[0x227] & 0x0FFFFFFFu;
    int u16 = (int)((p_regs[0x227] >> 31) & 1u);
    if (u16) return p_read16m(c, gpu_pica_phys_to_guest(base + off + i * 2u));
    return p_read8m(c, gpu_pica_phys_to_guest(base + off + i));
}

/* ---- Default-Attribut-/Immediate-Modus (0x232, Azahar SubmitImmediate) ---- */
static float g_vs_imm_attr[16][4];
static unsigned g_vs_imm_cur;
static uint32_t g_vs_imm_pack[4];
static unsigned g_vs_imm_pack_n;

/* Azahar PicaCore::DrawImmediate: der Immediate-Modus geht ebenfalls durch die
 * Geometrie-Pipeline (Reconfigure nur, wenn 0x232 seit dem letzten Flush
 * geschrieben wurde). */
static void p_vs_draw_immediate(ArmCpu *c)
{
    float outs[16][4];
    if (g_vs.code_words == 0u) return;   /* ohne Programm wird nichts gezeichnet */
    p_texcache_draw_start();
    p_vs_process(c, g_vs_imm_attr, outs);
    if (g_gp_imm_reset) {
        p_gs_pipeline_reconfigure();
        g_gp_imm_reset = 0;
    }
    if (g_gp.mode >= 0) {
        p_gs_pipeline_submit(c, (const float (*)[4])outs);
    } else {
        PicaVertex v;
        p_out_vertex((const float (*)[4])outs, &v);
        g_vs_last_vertex = v;
        g_vs_have_last = 1;
        p_prim_submit(c, &v);
    }
    g_vs_imm_cur = 0;
    if (!g_hw_render) gpu_pica_bump_tex_generation();
}

static int p_draw_arrays(ArmCpu *c, int indexed)
{
    static int plog = -1;
    static int tlog = -1;
    static unsigned draws = 0;
    static uint32_t tgt_addr = 0xFFFFFFFFu, tgt_w = 0, tgt_h = 0, tgt_fmt = 0xFFu;
    uint32_t nv = p_regs[0x228];
    uint32_t voff = p_regs[0x22a];
    uint32_t i;
    int use_shader = (g_vs.code_words > 0);

    if (plog < 0) { const char *e = getenv("HLE_PICA_LOG"); plog = (e && *e) ? atoi(e) : 0; }
    if (tlog < 0) { const char *e = getenv("HLE_PICA_TARGET"); tlog = (e && *e) ? atoi(e) : 0; }
    if (nv == 0) return 0;
    if (!use_shader) return 0;          /* Azahar: kein Draw ohne Vertex-Programm */
    if (nv > 0x10000u) nv = 0x10000u;
    g_pica_draws++;
    /* Diagnose (HLE_PICA_TARGET): jede Aenderung des PICA-Farbziels einmal
       melden. Zeigt, in welche Framebuffer das Spiel tatsaechlich rendert. */
    if (tlog > 0) {
        uint32_t a = (p_regs[0x11D] & 0x0FFFFFFFu) * 8u;
        uint32_t w = p_regs[0x11E] & 0x7FFu;
        uint32_t h = ((p_regs[0x11E] >> 12) & 0x3FFu) + 1u;
        uint32_t f = (p_regs[0x117] >> 16) & 7u;
        if (a != tgt_addr || w != tgt_w || h != tgt_h || f != tgt_fmt) {
            printf("[pica] target phys=%08X %ux%u fmt=%u draw#%u\n", a, w, h, f, draws);
            fflush(stdout);
            tgt_addr = a; tgt_w = w; tgt_h = h; tgt_fmt = f;
        }
    }
    p_texcache_draw_start();
    if (plog > 0 && draws < (unsigned)plog) {
        uint32_t base = ((p_regs[0x200] >> 1) & 0x0FFFFFFFu) * 16u;
        printf("[pica] draw#%u nv=%u voff=%u base=%08X attr0=%08X attr1=%08X attr2=%08X "
               "idx=%d tex0=%08X fmt=%08X vs=%d fb=%08X/%ux%u cfmt=%u\n", draws, nv, voff, base,
               p_regs[0x200], p_regs[0x201], p_regs[0x202], indexed,
               p_regs[0x81], p_regs[0x8e], use_shader,
               (p_regs[0x11D] & 0x0FFFFFFFu) * 8u,
               p_regs[0x11E] & 0x7FFu,
               ((p_regs[0x11E] >> 12) & 0x3FFu) + 1u,
               (p_regs[0x117] >> 16) & 7u);
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

    /* Azahar GeometryPipeline::Reconfigure + Setup je Draw. */
    p_gs_pipeline_reconfigure();

    for (i = 0; i < nv; i++) {
        uint32_t vert = indexed ? p_index_vertex(c, i) : (i + voff);
        float attrs[16][4], outs[16][4];

        /* VariablePrimitive: der Index ist die Vertexzahl des Batches. */
        if (indexed && p_gs_pipeline_need_index()) {
            p_gs_pipeline_submit_index(vert);
            continue;
        }
        p_load_attrs(c, vert, attrs);
        p_vs_process(c, attrs, outs);
        if (g_gp.mode >= 0) {
            p_gs_pipeline_submit(c, (const float (*)[4])outs);
        } else {
            PicaVertex v;
            p_out_vertex((const float (*)[4])outs, &v);
            g_vs_last_vertex = v;
            g_vs_have_last = 1;
            p_prim_submit(c, &v);
        }
    }
    draws++;
    if (!g_hw_render) gpu_pica_bump_tex_generation();
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
   oder 4 Woerter float32 je Vec4. Der Index steht im uniform_setup-Register
   der Einheit (0x2C0 VS, 0x290 GS; Bit31 = Format) und wird nach jedem
   vollstaendigen Vec4 um eins erhoeht (7 Bit, Indizes >= 96 ignoriert). */
static int p_uniform_push(PicaShader *st, uint32_t idxreg, uint32_t word)
{
    int is32 = (int)((p_regs[idxreg] >> 31) & 1u);
    unsigned need = is32 ? 4u : 3u;
    unsigned idx;
    int complete = 0;

    st->pack[st->pack_n++] = word;
    if (st->pack_n < need) return 0;

    idx = p_regs[idxreg] & 0x7Fu;
    if (idx < 96u) {
        int i;
        if (is32) {
            for (i = 0; i < 4; i++) {
                float f;
                memcpy(&f, &st->pack[i], 4);
                st->f[idx][3 - i] = f;
            }
        } else {
            p_unpack_f24(st->pack[0], st->pack[1], st->pack[2], st->f[idx]);
        }
        p_regs[idxreg] = (p_regs[idxreg] & ~0x7Fu) | ((idx + 1u) & 0x7Fu);
        complete = 1;
    }
    st->pack_n = 0;
    return complete ? (int)(p_regs[idxreg] & 0x7Fu) : 0;
}

/* Default-Attribute/Immediate: Azahar PicaCore::SubmitImmediate. */
static void p_vs_submit_immediate(ArmCpu *c, uint32_t word)
{
    unsigned idx = p_regs[0x232];      /* Azahar: setup.index ist ein voller u32 */
    float a[4];

    g_vs_imm_pack[g_vs_imm_pack_n++] = word;
    if (g_vs_imm_pack_n < 3u) return;
    p_unpack_f24(g_vs_imm_pack[0], g_vs_imm_pack[1], g_vs_imm_pack[2], a);
    g_vs_imm_pack_n = 0;

    if (idx > 0xFu) return;            /* Azahar: Invalid VS default attribute index */
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

/* Gemeinsames Spezialregister-Schema einer Shader-Einheit (Azahar
   HandleSpecialReg): base = 0x2B0 (VS) bzw. 0x280 (GS/Einheit 3).
   `code_limit` ist das adressierbare Programmfenster (VS 512, GS 4096).
   Rueckgabe: 1-basierter Schreibindex fuer Uniform-/Programm-/Swizzle-
   Uploads (sonst 0), damit der gemeinsame VS/GS-Pfad genau die Worte
   spiegeln kann, die Azahar zusaetzlich nach gs_setup kopiert. */
static int p_shader_reg_write(PicaShader *st, uint32_t base, uint32_t id,
                              uint32_t value, unsigned code_limit)
{
    uint32_t off = id - base;

    if (off >= 0x11u && off <= 0x18u) {             /* uniform_setup.set_value */
        int idx = p_uniform_push(st, base + 0x10u, value);
        return idx > 0 ? idx : 0;
    }
    if (off >= 0x1Cu && off <= 0x23u) {             /* program.set_word */
        uint32_t *poff = &p_regs[base + 0x1Bu];
        if (*poff < code_limit) {
            st->code[*poff] = value;
            if (*poff + 1u > st->code_words) st->code_words = *poff + 1u;
            *poff = *poff + 1u;
            return (int)*poff;
        }
        return 0;
    }
    if (off >= 0x26u && off <= 0x2Du) {             /* swizzle.set_word */
        uint32_t *soff = &p_regs[base + 0x25u];
        if (*soff < VS_SWZ_WORDS) {
            st->swz[*soff] = value;
            *soff = *soff + 1u;
            return (int)*soff;
        }
        return 0;
    }
    if (off == 0u) {                                /* bool_uniforms */
        unsigned i;
        uint32_t reg = p_regs[id];       /* Azahar: maskierter Registerwert */
        for (i = 0; i < 16u; i++) st->b[i] = (unsigned char)((reg >> i) & 1u);
    } else if (off >= 1u && off <= 4u) {            /* int_uniforms */
        uint32_t reg = p_regs[id];       /* Azahar: maskierter Registerwert */
        memcpy(st->i[off - 1u], &reg, 4);
    }
    return 0;
}

/* Spezialregister des Vertex-Shaders/der Geometrie-Einheit (Azahar
   HandleSpecialReg). Wird nach dem maskierten Register-Schreiben mit dem
   Rohwert aufgerufen. Solange 0x244 (gs_unit_exclusive_configuration) = 0 ist
   und der GS nicht benutzt wird (0x229 use_gs == 0), teilt sich Einheit 3 Programm,
   Swizzle, Uniforms, Bool-/Int-Uniforms und output_mask mit dem VS
   (pica_core.cpp: die vs.*-Handler kopieren nach gs_setup). */
static void p_vs_reg_write(ArmCpu *c, uint32_t id, uint32_t value)
{
    int shared = !(p_regs[0x244] & 1u) && (p_regs[0x229] & 3u) == 0u;
    int n;

    if (id == 0x232u) {                       /* Default-Attribut-Index */
        g_vs_imm_pack_n = 0;
        g_vs_imm_cur = 0;
        g_gp_imm_reset = 1;                    /* Azahar immediate.Reset() */
        return;
    }
    if (id >= 0x233u && id <= 0x235u) {        /* set_value (f24-Queue) */
        p_vs_submit_immediate(c, value);
        return;
    }
    if (id >= 0x280u && id <= 0x2AFu) {
        p_shader_reg_write(&g_gs, 0x280u, id, value, VS_PROG_WORDS);
        return;
    }
    if (id < 0x2B0u || id > 0x2DFu) return;

    n = p_shader_reg_write(&g_vs, 0x2B0u, id, value, VS_REG_WORDS);
    if (!shared) return;
    if (id == 0x2B0u) {                       /* bool_uniforms */
        memcpy(g_gs.b, g_vs.b, sizeof(g_gs.b));
    } else if (id >= 0x2B1u && id <= 0x2B4u) { /* int_uniforms */
        memcpy(g_gs.i[id - 0x2B1u], g_vs.i[id - 0x2B1u], 4);
    } else if (id >= 0x2C1u && id <= 0x2C8u) { /* fertig geschriebener Vec4 */
        if (n > 0) memcpy(g_gs.f[n - 1], g_vs.f[n - 1], 4 * sizeof(float));
    } else if (id >= 0x2CCu && id <= 0x2D3u) { /* Programmwort */
        if (n > 0) {
            g_gs.code[n - 1] = g_vs.code[n - 1];
            if ((unsigned)n > g_gs.code_words) g_gs.code_words = (unsigned)n;
        }
    } else if (id >= 0x2D6u && id <= 0x2DDu) { /* Swizzle-Muster */
        if (n > 0) g_gs.swz[n - 1] = g_vs.swz[n - 1];
    } else if (id == 0x2BDu) {                /* output_mask */
        /* Azahar: regs.gs.output_mask.Assign(value) -- BitField<0,16> mit dem
         * ROHWERT der Schreibung; die oberen Bits von 0x28D bleiben. */
        p_regs[0x28D] = (p_regs[0x28D] & ~0xFFFFu) | (value & 0xFFFFu);
    }
}

/* ---- Test-/Diagnosezugriff (keine Spielsemantik) ---- */
static void p_vs_reset_draw_state(void)
{
    g_vs_imm_pack_n = 0;
    g_vs_imm_cur = 0;
    g_prim_topology = 0;              /* PipelineRegs::TriangleTopology::List */
    p_prim_reset();
    g_vs_have_last = 0;
    memset(&g_emit, 0, sizeof(g_emit));
    g_gp.mode = -1;
    g_gp.cur = 0;
    g_gp.end = 0;
    g_gp_imm_reset = 1;
    g_gs_emit_mask = 0;
}

float gpu_pica_vs_uniform(int index, int comp)
{
    if (index < 0 || index >= 96 || comp < 0 || comp >= 4) return 0.0f;
    return g_vs.f[index][comp];
}

int gpu_pica_vs_active(void) { return g_vs.code_words > 0; }

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
    int jump_pending;        /* 0x23C/0x23D: aktuelle Liste wurde ersetzt */
    uint32_t jump_addr;      /* physische Adresse des Ziel-Kanals */
    uint32_t jump_size;
} PicaCtx;

/* Diagnose (HLE_PICA_DIAG): Histogramm der geschriebenen Register-IDs je
 * Liste plus Rohdump. Reines Lesen/Log, veraendert keinen Gastzustand. */
static uint32_t g_diag_hist[PICA_NREGS];
static int g_diag_jumps;

static int p_diag_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("HLE_PICA_DIAG");
        mode = (e && *e) ? atoi(e) : 0;
    }
    return mode;
}

static void p_diag_dump(ArmCpu *c, uint32_t addr, uint32_t size)
{
    char path[128];
    FILE *f;
    uint32_t i, words = size / 4u;

    snprintf(path, sizeof(path), "scratch/pica/list_%08X_%X.bin", addr, size);
    f = fopen(path, "wb");
    if (!f) return;
    for (i = 0; i < words; i++) {
        uint32_t v = p_read32(c, addr + i * 4u);
        unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                               (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
        fwrite(b, 1, 4, f);
    }
    fclose(f);
}

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
    if (p_diag_mode()) g_diag_hist[id]++;

    /* Beleuchtung ist je Draw konstant (0x140..0x1D9); jede Aenderung
       verwirft den vorberechneten Draw-Zustand. */
    if (id >= 0x140u && id <= 0x1D9u) p_light_draw_invalidate();
    p_vs_reg_write(c, id, value);
    if (id >= 0xE8u && id <= 0xEFu) p_fog_lut_write(value);
    if (id >= 0x1C8u && id <= 0x1CFu) p_light_lut_write(value);
    if (id >= 0xB0u && id <= 0xB7u) p_proctex_lut_write(value);

    if (id == 0x010u) {             /* Finalize / irq_request */
        /* Azahar: every_byte_match(irq_request, irq_compare) -> P3D; bei
           irq_autostop endet die Liste hier (irq_autostop ist ein ganzer u32,
           nicht nur Bit0). */
        if (p_any_byte_match(p_regs[0x010], p_regs[0x020])) {
            if (p_irq_handler) p_irq_handler(5);
            if (p_regs[0x034] != 0u) ctx->stop = 1;
        }
    } else if (id == 0x22Eu) {
        p_draw_arrays(c, 0);
    } else if (id == 0x22Fu) {
        p_draw_arrays(c, 1);
    } else if (id == 0x25Eu) {      /* triangle_topology (special) */
        g_prim_topology = (int)((p_regs[0x25E] >> 8) & 3u);
        p_prim_reset();             /* Azahar: Reconfigure() setzt zurueck */
    } else if (id == 0x25Fu) {      /* restart_primitive (special) */
        p_prim_reset();
    } else if (id == 0x23Cu || id == 0x23Du) {
        uint32_t ch = id - 0x23Cu;
        /* Azahar HandleSpecialReg: jeder Schreibzugriff macht cmd_list.Reset,
         * unabhaengig vom geschriebenen Wert und ohne Gueltigkeitspruefung.
         * Die alte Liste endet also immer; ist die neue Liste leer oder nicht
         * abbildbar, laeuft nichts weiter (kein P3D -- das meldet nur
         * GPU::SubmitCmdList fuer den GSP-Trigger). Der Trigger bleibt im
         * Register stehen, weil hier der Register-Array-Wert nicht
         * zurueckgesetzt wird. */
        ctx->jump_addr = (p_regs[0x23Au + ch] & 0x0FFFFFFFu) * 8u;
        ctx->jump_size = (p_regs[0x238u + ch] & 0x000FFFFFu) * 8u;
        ctx->jump_pending = 1;
        if (p_diag_mode()) g_diag_jumps++;
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
        if (ctx->stop || ctx->jump_pending) break;

        for (uint32_t k = 0; k < extra; k++) {
            uint32_t ev, eid;
            if (i >= words) break;
            ev = p_read32(c, a + i * 4u);
            eid = group ? (id + 1u + k) : id;
            i++;
            p_write_reg(c, eid, ev, mask, ctx);
            if (ctx->stop || ctx->jump_pending) break;
            /* Nach einem ungeraden Record folgt ein Pad-Wort; das erledigt
               die Ausrichtung am Schleifenkopf. */
        }
    }

    if (ctx->jump_pending) {
        uint32_t ja = ctx->jump_addr;
        uint32_t js = ctx->jump_size;
        ctx->jump_pending = 0;
        ctx->jump_addr = 0;
        ctx->jump_size = 0;
        /* Azahar cmd_list.Reset: die neue Liste ersetzt die alte. Nur eine
         * nicht leere, abbildbare Liste wird ausgefuehrt; sonst endet die
         * Verarbeitung hier (kein P3D, siehe p_write_reg). */
        if (js != 0u
                && (uint64_t)gpu_pica_phys_to_guest(ja) + js <= c->mem_size)
            p_run_list(c, gpu_pica_phys_to_guest(ja), js, ctx, depth + 1);
    }
}

int gpu_run_pica_list(ArmCpu *c, uint32_t addr, uint32_t size)
{
    PicaCtx ctx;
    int diag = p_diag_mode();
    unsigned long long draws0 = g_pica_draws;

    p_hw_tex_gen_bump();       /* neue Liste: Texturen konservativ neu pruefen */
    /* Gast-CPU kann zwischen zwei Listen in Farb-/Tiefenziel geschrieben
       haben. hw_list_begin vergleicht mit der Schattenkopie und laedt nur
       dann neu (sonst wuerde der Flush am Listenende die CPU-Bytes mit dem
       alten D3D-Stand ueberschreiben). */
    if (g_hw_render) hw_list_begin();
    memset(&ctx, 0, sizeof(ctx));
    if (diag) {
        memset(g_diag_hist, 0, sizeof(g_diag_hist));
        g_diag_jumps = 0;
    }
    {
        /* Die Gast-VFP stellt die Host-Rundung des Threads auf den FPSCR-
           Modus (3DS: round-toward-zero). Die PICA-Pipeline ist Hardware
           mit eigener Arithmetik; der Software-Renderer rechnet daher immer
           mit Rundung zum Naechsten und stellt den Gastmodus danach wieder
           her. */
        int saved_round = fegetround();
        if (saved_round != FE_TONEAREST) fesetround(FE_TONEAREST);
        p_run_list(c, gpu_pica_phys_to_guest(addr), size, &ctx, 0);
        if (saved_round != FE_TONEAREST) fesetround(saved_round);
    }
    /* Ende der Kommandoliste: der Gast darf die Ergebnisse lesen. */
    if (g_hw_render) hw_flush_to_guest();
    g_pica_lists++;
    if (diag) {
        int i, first = 1;
        printf("[picadiag] list addr=%08X size=%u ops=%d jumps=%d draws=%d ids:",
               addr, size, ctx.ops, g_diag_jumps,
               (int)(g_pica_draws - draws0));
        for (i = 0; i < PICA_NREGS; i++) {
            if (!g_diag_hist[i]) continue;
            printf("%s%03X=%u", first ? " " : ",", i, g_diag_hist[i]);
            first = 0;
        }
        printf("\n");
        fflush(stdout);
        p_diag_dump(c, gpu_pica_phys_to_guest(addr), size);
    }
    return ctx.ops;
}



/* ================= Speichertransfers (GSP/PICA) ========================= */
/* --- Pixelformate und Speichertransfers (Azahar renderer_software/
 * sw_blitter.cpp, video_core/utils.h) --- */
typedef struct { uint8_t r, g, b, a; } GpuColor;

static int p_bpp(uint32_t fmt) {
    switch (fmt & 7u) {
        case 0: return 4;                 /* RGBA8 */
        case 1: return 3;                 /* RGB8  */
        case 2: case 3: case 4: return 2; /* RGB565, RGB5A1, RGBA4 */
        default: return 0;
    }
}

static uint8_t p_5to8(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static uint8_t p_6to8(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static uint8_t p_4to8(uint32_t v) { return (uint8_t)((v << 4) | v); }

static GpuColor p_decode_pixel(uint32_t fmt, const uint8_t *p) {
    GpuColor col = {0, 0, 0, 0};
    switch (fmt & 7u) {
        case 0: /* RGBA8 */
            col.r = p[3]; col.g = p[2]; col.b = p[1]; col.a = p[0];
            break;
        case 1: /* BGR8 */
            col.r = p[2]; col.g = p[1]; col.b = p[0]; col.a = 255;
            break;
        case 2: { /* RGB565 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = p_5to8((v >> 11) & 0x1Fu);
            col.g = p_6to8((v >> 5) & 0x3Fu);
            col.b = p_5to8(v & 0x1Fu); col.a = 255;
            break;
        }
        case 3: { /* RGB5A1 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = p_5to8((v >> 11) & 0x1Fu);
            col.g = p_5to8((v >> 6) & 0x1Fu);
            col.b = p_5to8((v >> 1) & 0x1Fu);
            col.a = (uint8_t)((v & 1u) * 255u);
            break;
        }
        case 4: { /* RGBA4 */
            uint32_t v; memcpy(&v, p, 2);
            col.r = p_4to8((v >> 12) & 0xFu);
            col.g = p_4to8((v >> 8) & 0xFu);
            col.b = p_4to8((v >> 4) & 0xFu);
            col.a = p_4to8(v & 0xFu);
            break;
        }
        default: break;
    }
    return col;
}

static void p_encode_pixel(uint32_t fmt, uint8_t *p, GpuColor col) {
    switch (fmt & 7u) {
        case 0:
            p[3] = col.r; p[2] = col.g; p[1] = col.b; p[0] = col.a;
            break;
        case 1:
            p[2] = col.r; p[1] = col.g; p[0] = col.b;
            break;
        case 2: {
            uint32_t v = ((uint32_t)(col.r >> 3) << 11) |
                         ((uint32_t)(col.g >> 2) << 5) | (col.b >> 3);
            memcpy(p, &v, 2);
            break;
        }
        case 3: {
            uint32_t v = ((uint32_t)(col.r >> 3) << 11) |
                         ((uint32_t)(col.g >> 3) << 6) |
                         ((uint32_t)(col.b >> 3) << 1) | (col.a >> 7);
            memcpy(p, &v, 2);
            break;
        }
        case 4: {
            uint32_t v = ((uint32_t)(col.r >> 4) << 12) |
                         ((uint32_t)(col.g >> 4) << 8) |
                         ((uint32_t)(col.b >> 4) << 4) | (col.a >> 4);
            memcpy(p, &v, 2);
            break;
        }
        default: break;
    }
}

/* 8x8-Morton-Offset (Z-Order) wie Azahar VideoCore::GetMortonOffset. */

/* DisplayTransfer (Azahar SwBlitter::DisplayTransfer):
 * Pixelformat-Konvertierung und Umsetzung zwischen linearer und gekachelter
 * (8x8-Morton) Anordnung. Flags: Bit0 flip, Bit1 input_linear,
 * Bit2 crop_input_lines, Bit5 dont_swizzle, Bits8..10 input_format,
 * Bits12..14 output_format, Bits24..25 scaling. */
/* Azahar SwBlitter::DisplayTransfer. Adressen sind bereits Gast-Linear-
 * Adressen (GSP/PICA-Transfers arbeiten mit physischen Adressen). */
static void p_display_transfer_run(ArmCpu *c, uint32_t in_va, uint32_t out_va,
                                   uint32_t in_size, uint32_t out_size, uint32_t flags) {
    const uint32_t in_w = in_size & 0xFFFFu, in_h = (in_size >> 16) & 0xFFFFu;
    const uint32_t out_w = out_size & 0xFFFFu, out_h = (out_size >> 16) & 0xFFFFu;
    const uint32_t in_fmt = (flags >> 8) & 7u, out_fmt = (flags >> 12) & 7u;
    const int flip = (flags & 1u) != 0u;
    const int in_linear = (flags & 2u) != 0u;
    const int crop = (flags & 4u) != 0u;
    const int dont_swizzle = (flags & 0x20u) != 0u;
    const uint32_t scaling = (flags >> 24) & 3u;
    const int in_bpp = p_bpp(in_fmt), out_bpp = p_bpp(out_fmt);
    uint32_t src, dst, hs, vs, ow, oh, y, x;

    if (in_w == 0u || in_h == 0u || out_w == 0u || out_h == 0u) return;
    if (in_bpp == 0 || out_bpp == 0) {
        printf("[gspq] DisplayTransfer Format %u/%u ungueltig\n", in_fmt, out_fmt);
        return;
    }
    if (scaling > 2u || (in_linear && scaling != 0u)) {
        printf("[gspq] DisplayTransfer Scaling %u ungueltig (linear=%d)\n", scaling, in_linear);
        return;
    }

    src = in_va & 0x1FFFFFFFu;
    dst = out_va & 0x1FFFFFFFu;
    if (flip && crop)
        dst += (in_w - out_w) * (out_h - 1u) * (uint32_t)out_bpp;
    if ((uint64_t)src + (uint64_t)in_w * in_h * (uint32_t)in_bpp > c->mem_size ||
        (uint64_t)dst + (uint64_t)out_w * out_h * (uint32_t)out_bpp > c->mem_size) {
        printf("[gspq] DisplayTransfer ausserhalb: %08X -> %08X\n", in_va, out_va);
        return;
    }

    hs = scaling != 0u ? 1u : 0u;
    vs = scaling == 2u ? 1u : 0u;
    ow = out_w >> hs;
    oh = out_h >> vs;
    if (ow == 0u || oh == 0u) return;

    for (y = 0; y < oh; y++) {
        for (x = 0; x < ow; x++) {
            const uint32_t ix = x << hs, iy = y << vs;
            const uint32_t oy = flip ? (oh - y - 1u) : y;
            uint32_t so, dof;
            GpuColor col;

            if (in_linear) {
                if (!dont_swizzle) {
                    so = (ix + iy * in_w) * (uint32_t)in_bpp;
                    dof = p_morton_offset(x, oy, (uint32_t)out_bpp) +
                          (oy & ~7u) * ow * (uint32_t)out_bpp;
                } else {
                    so = (ix + iy * in_w) * (uint32_t)in_bpp;
                    dof = (x + oy * ow) * (uint32_t)out_bpp;
                }
            } else {
                if (!dont_swizzle) {
                    so = p_morton_offset(ix, iy, (uint32_t)in_bpp) +
                         (iy & ~7u) * in_w * (uint32_t)in_bpp;
                    dof = (x + oy * ow) * (uint32_t)out_bpp;
                } else {
                    so = p_morton_offset(ix, iy, (uint32_t)in_bpp) +
                         (iy & ~7u) * in_w * (uint32_t)in_bpp;
                    dof = p_morton_offset(x, oy, (uint32_t)out_bpp) +
                          (oy & ~7u) * ow * (uint32_t)out_bpp;
                }
            }
            if ((uint64_t)src + so + (uint32_t)in_bpp > c->mem_size ||
                (uint64_t)dst + dof + (uint32_t)out_bpp > c->mem_size)
                continue;

            col = p_decode_pixel(in_fmt, c->mem + src + so);
            if (scaling == 1u) {
                GpuColor p1 = p_decode_pixel(in_fmt, c->mem + src + so + (uint32_t)in_bpp);
                col.r = (uint8_t)((col.r + p1.r) / 2u);
                col.g = (uint8_t)((col.g + p1.g) / 2u);
                col.b = (uint8_t)((col.b + p1.b) / 2u);
                col.a = (uint8_t)((col.a + p1.a) / 2u);
            } else if (scaling == 2u) {
                const uint32_t b = (uint32_t)in_bpp;
                GpuColor p1 = p_decode_pixel(in_fmt, c->mem + src + so + b);
                GpuColor p2 = p_decode_pixel(in_fmt, c->mem + src + so + 2u * b);
                GpuColor p3 = p_decode_pixel(in_fmt, c->mem + src + so + 3u * b);
                col.r = (uint8_t)((col.r + p1.r + p2.r + p3.r) / 4u);
                col.g = (uint8_t)((col.g + p1.g + p2.g + p3.g) / 4u);
                col.b = (uint8_t)((col.b + p1.b + p2.b + p3.b) / 4u);
                col.a = (uint8_t)((col.a + p1.a + p2.a + p3.a) / 4u);
            }
            p_encode_pixel(out_fmt, c->mem + dst + dof, col);
        }
    }
}

/* Command 0x04: TextureCopy (Azahar SwBlitter::TextureCopy): kopiert
 * 16-Byte-Zeilen mit optionalem Quell-/Ziel-Gap. */
static void p_texture_copy_run(ArmCpu *c, uint32_t src, uint32_t dst, uint32_t size,
                               uint32_t in_w_raw, uint32_t in_gap_raw,
                               uint32_t out_w_raw, uint32_t out_gap_raw) {
    uint32_t rem = size & ~0xFu;
    uint32_t in_gap = in_gap_raw * 16u;
    uint32_t out_gap = out_gap_raw * 16u;
    /* Null-Gap heisst zusammenhaengend; dann ist die Breite die Gesamtgroesse
       (Azahar SwBlitter::TextureCopy). */
    uint32_t in_width = (in_gap == 0u) ? rem : in_w_raw * 16u;
    uint32_t out_width = (out_gap == 0u) ? rem : out_w_raw * 16u;
    uint32_t ri, ro;
    uint64_t need_in, need_out;

    src &= 0x1FFFFFFFu;
    dst &= 0x1FFFFFFFu;
    if (rem == 0u || in_width == 0u || out_width == 0u) return;
    need_in = (uint64_t)(rem / in_width) * (in_width + in_gap);
    need_out = (uint64_t)(rem / out_width) * (out_width + out_gap);
    if ((uint64_t)src + need_in > c->mem_size || (uint64_t)dst + need_out > c->mem_size) {
        printf("[gspq] TextureCopy ausserhalb: %08X -> %08X (%X)\n", src, dst, rem);
        return;
    }

    ri = in_width;
    ro = out_width;
    while (rem > 0u) {
        uint32_t n = ri < ro ? ri : ro;
        if (n > rem) n = rem;
        memmove(c->mem + dst, c->mem + src, n);
        src += n; dst += n;
        ri -= n; ro -= n; rem -= n;
        if (ri == 0u) { ri = in_width; src += in_gap; }
        if (ro == 0u) { ro = out_width; dst += out_gap; }
    }
}

/* ================= GPU-MMIO-Registerfeld (GSP 0x0001..0x0004) =============
 *
 * Azahar VideoCore::GPU::ReadReg/WriteReg (video_core/gpu.cpp). Der
 * GSP-Client erreicht die GPU ueber die Basis 0x1EB00000 (REGS_BEGIN in
 * hle/service/gsp/gsp_gpu.cpp); die Adressen der Schreibbefehle sind
 * Offsets davon. Drei Bereiche:
 *
 *   0x1EF00000..0x1EF00FFF  externe PICA-Register (Regs vor `internal`):
 *                           MemoryFill 0x010..0x02F, Framebuffer 0x400..0x5FF,
 *                           DisplayTransfer/TextureCopy 0xC00..0xC2F
 *   0x1EF01000..0x1EF01BFF  interne PICA-Register (IDs 0x000..0x2FF,
 *                           dieselben, die Kommandolisten schreiben)
 *   0x1ED02000..0x1ED02A3F  LCD (RegsLcd: ColorFill/Backlight)
 *
 * Schreiben speichern den Wert; Trigger-Bits starten die Operation und
 * werden danach zurueckgesetzt (GPU::WriteReg -> MemoryFill/MemoryTransfer):
 *   0x01C control.trigger  -> MemoryFill 0, danach PSC0
 *   0x02C control.trigger  -> MemoryFill 1, danach PSC1
 *   0xC18 transfer.trigger -> DisplayTransfer/TextureCopy, danach PPF
 *   0x123C/0x123D          -> Kommandoliste von Kanal 0/1 (SubmitCmdList)
 * Fertig-Bit: MemoryFill control Bit1 wird nach Abschluss gesetzt.
 */
#define GPU_MMIO_VADDR   0x1EF00000u
#define GPU_MMIO_LCD     0x1ED02000u
/* RegsLcd::NumIds(): 0x81 + 1 + 0xE + 1 + 0x1F0 + 1 + 0xE + 1 + 0x16F
 * = 0x400 Woerter (die LCD-Seite ist genau 4 KiB). */
#define GPU_MMIO_LCD_W   0x400u
#define GPU_MMIO_EXT_W   0x400u    /* 0x1EF00000..0x1EF00FFF */

static uint32_t p_ext_regs[GPU_MMIO_EXT_W];
static uint32_t p_lcd_regs[GPU_MMIO_LCD_W];
static int p_mmio_init_done;

/* Azahar PicaCore::InitializeRegs: Framebuffer-Defaults von nn::gx
 * (nur fuer Registerleser; die Anzeige kommt aus der GSP-FrameBufferInfo). */
static void p_mmio_init(void)
{
    if (p_mmio_init_done) return;
    p_mmio_init_done = 1;
    p_ext_regs[0x117] = 240u | (400u << 16);   /* fb0 width/height */
    p_ext_regs[0x11A] = 0x181E6000u;
    p_ext_regs[0x11B] = 0x1822C800u;
    p_ext_regs[0x11C] = 1u;                    /* RGB8 */
    p_ext_regs[0x124] = 3u * 240u;             /* stride */
    p_ext_regs[0x125] = 0x18273000u;
    p_ext_regs[0x126] = 0x182B9800u;
    p_ext_regs[0x157] = 240u | (320u << 16);   /* fb1 width/height */
    p_ext_regs[0x15A] = 0x1848F000u;
    p_ext_regs[0x15B] = 0x184C7800u;
    p_ext_regs[0x15C] = 1u;
    p_ext_regs[0x164] = 3u * 240u;
}

static uint32_t p_merge(uint32_t old, uint32_t value, uint32_t mask)
{
    return (old & ~mask) | (value & mask);
}


/* Azahar SwBlitter::MemoryFill (sw_blitter.cpp): 16/24/32 Bit Wert fuellen.
 * start/end sind bereits Gast-Linearadressen. */
static void p_mem_fill(ArmCpu *c, uint32_t start, uint32_t end,
                       uint32_t value, uint32_t control)
{
    /* Der Port legt Gastadressen unter (addr & 0x1FFFFFFF) im Hostpuffer ab
       (wie arm_mem_read/write und p_read32). */
    uint64_t so = start & 0x1FFFFFFFu, eo = end & 0x1FFFFFFFu, off;

    if (start == 0u || so >= eo || eo > c->mem_size) return;
    if (control & 0x200u) {
        for (off = so; off + 4u <= eo; off += 4u)
            memcpy(c->mem + off, &value, 4);
    } else if (control & 0x100u) {
        for (off = so; off + 3u <= eo; off += 3u) {
            c->mem[off] = (uint8_t)(value & 0xFFu);
            c->mem[off + 1u] = (uint8_t)((value >> 8) & 0xFFu);
            c->mem[off + 2u] = (uint8_t)((value >> 16) & 0xFFu);
        }
    } else {
        uint16_t v16 = (uint16_t)(value & 0xFFFFu);
        for (off = so; off + 2u <= eo; off += 2u)
            memcpy(c->mem + off, &v16, 2);
    }
}

/* MemoryFill aus den Registern. Azahar GPU::MemoryFill: nur bei
 * control.trigger; Interrupt nur wenn die Startadresse != 0 ist; danach
 * trigger=0 und finished=1 -- auch wenn der Bereich ungueltig ist. */
static void p_ext_memory_fill(ArmCpu *c, int index, int intr_index)
{
    uint32_t *f = &p_ext_regs[4u + (uint32_t)index * 4u];
    uint32_t start = f[0] * 8u, end = f[1] * 8u;
    uint32_t value = f[2], control = f[3];

    if ((control & 1u) == 0u) return;
    /* Vor dem Gast-Schreiben ausstehende HW-Ergebnisse sichern und das
       Ziel verwerfen (der naechste HW-Draw laedt neu aus dem Gast). */
    hw_sync_guest();
    p_hw_tex_gen_bump();
    {
        uint32_t gs = gpu_pica_phys_to_guest(start);
        uint32_t ge = gpu_pica_phys_to_guest(end);
        if (ge > gs) gpu_pica_invalidate_tex_range(gs, ge - gs);
    }
    p_mem_fill(c, gpu_pica_phys_to_guest(start), gpu_pica_phys_to_guest(end),
               value, control);
    f[3] = (control & ~1u) | 2u;
    if (start != 0u && intr_index >= 0 && p_irq_handler)
        p_irq_handler(intr_index);      /* PSC0/PSC1 */
}

/* Azahar GPU::MemoryTransfer: Trigger pruefen, Transfer ausfuehren,
 * Trigger loeschen, PPF melden. */
static int p_gspq_log(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("HLE_GSPQ_LOG"); v = (e && *e) ? atoi(e) : 0; }
    return v;
}

/* Laenge des von p_texture_copy_run beschriebenen Zielbereichs ab dst,
 * inklusive der Output-Gaps zwischen den Zeilen (Azahar SwBlitter::
 * TextureCopy). Ohne Gaps ist das die Kopiergroesse. */
static uint32_t p_texture_copy_extent(uint32_t size, uint32_t out_w_raw,
                                      uint32_t out_gap_raw)
{
    uint32_t rem = size & ~0xFu;
    uint32_t out_gap = out_gap_raw * 16u;
    uint32_t out_width = (out_gap == 0u) ? rem : out_w_raw * 16u;
    uint64_t n;
    if (rem == 0u || out_width == 0u) return 0u;
    n = (uint64_t)(rem / out_width) * (out_width + out_gap) + rem % out_width;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

/* Laenge des von p_display_transfer_run beschriebenen Zielbereichs ab dem
 * Ziel-Startwert (Flip+Crop verschiebt den Start). */
static uint32_t p_display_transfer_extent(uint32_t in_size, uint32_t out_size,
                                          uint32_t flags)
{
    uint32_t in_w = in_size & 0xFFFFu;
    uint32_t out_w = out_size & 0xFFFFu, out_h = (out_size >> 16) & 0xFFFFu;
    uint64_t bpp = (uint64_t)p_bpp((flags >> 12) & 7u), n;
    if (out_w == 0u || out_h == 0u || bpp == 0u) return 0u;
    n = (uint64_t)out_w * out_h * bpp;
    if ((flags & 1u) && (flags & 4u))
        n += (uint64_t)(uint32_t)(in_w - out_w) * (out_h - 1u) * bpp;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)n;
}

static void p_ext_transfer(ArmCpu *c)
{
    uint32_t in_p, out_p, flags;

    if ((p_ext_regs[0x306] & 1u) == 0u) return;
    in_p = p_ext_regs[0x300] * 8u;
    out_p = p_ext_regs[0x301] * 8u;
    flags = p_ext_regs[0x304];
    if (p_gspq_log())
        printf("[gpu] transfer in=%08X out=%08X flags=%08X sz=%08X/%08X tc=%u\n",
               in_p, out_p, flags, p_ext_regs[0x303], p_ext_regs[0x302],
               (flags >> 3) & 1u);
    /* Vorher ausstehende HW-Ergebnisse sichern (der Flush erhoeht die
       Texturgeneration) und das Ziel verwerfen. Die Kopie selbst markiert
       nur die Cache-Eintraege im tatsaechlich beschriebenen Zielbereich. */
    hw_sync_guest();
    if (flags & 8u) {                   /* is_texture_copy */
        /* Azahar regs_external.h DisplayTransferConfig: flags@0x304,
           trigger@0x306, texture_copy.size@0x308, input_size@0x309,
           output_size@0x30A. */
        uint32_t dst = gpu_pica_phys_to_guest(out_p);
        uint32_t sz = p_ext_regs[0x308];
        /* Mit Output-Gap ist der beschriebene Bereich laenger als sz. */
        gpu_pica_invalidate_tex_range(dst, p_texture_copy_extent(
            sz, p_ext_regs[0x30A] & 0xFFFFu, (p_ext_regs[0x30A] >> 16) & 0xFFFFu));
        p_texture_copy_run(c, gpu_pica_phys_to_guest(in_p),
                           dst, sz,
                           p_ext_regs[0x309] & 0xFFFFu,
                           (p_ext_regs[0x309] >> 16) & 0xFFFFu,
                           p_ext_regs[0x30A] & 0xFFFFu,
                           (p_ext_regs[0x30A] >> 16) & 0xFFFFu);
    } else {
        uint32_t dst = gpu_pica_phys_to_guest(out_p);
        /* output_size (0x302) ist Breite|Hoehe<<16 in Pixeln. */
        gpu_pica_invalidate_tex_range(dst, p_display_transfer_extent(
            p_ext_regs[0x303], p_ext_regs[0x302], flags));
        p_display_transfer_run(c, gpu_pica_phys_to_guest(in_p),
                               dst,
                               p_ext_regs[0x303], p_ext_regs[0x302], flags);
    }
    p_ext_regs[0x306] &= ~1u;
    if (p_irq_handler) p_irq_handler(4);   /* PPF */
}

/* Azahar GPU::SubmitCmdList ueber die Register (Trigger 0x23C/0x23D).
 * Gueltigkeitspruefung wie GPU::SubmitCmdList: Laenge 0 ist zulaessig und
 * fuehrt nichts aus, sonst muss der Bereich im Gast-Speicher liegen (P3D).
 * Adresse 0 ist physisch gueltiger FCRAM. */
static int p_ext_submit_cmdlist(ArmCpu *c, uint32_t ch)
{
    uint32_t paddr = (p_regs[0x23Au + ch] & 0x0FFFFFFFu) * 8u;
    uint32_t size = (p_regs[0x238u + ch] & 0x000FFFFFu) * 8u;

    if (size != 0u
            && (uint64_t)gpu_pica_phys_to_guest(paddr) + size > c->mem_size) {
        if (p_irq_handler) p_irq_handler(5);   /* ungueltige Liste -> P3D */
        return 0;
    }
    return gpu_run_pica_list(c, paddr, size);
}

/* Azahar GPU::Execute CommandId::SubmitCmdList: Adresse/Groesse in die
 * Kommandopuffer-Register schreiben, Trigger setzen, ausfuehren und den
 * Trigger wieder loeschen (GPU::SubmitCmdList). */
int gpu_gsp_submit_cmdlist_cmd(ArmCpu *c, uint32_t paddr, uint32_t size)
{
    int ops;

    p_mmio_init();
    p_regs[0x238] = (size >> 3) & 0x000FFFFFu;
    p_regs[0x23A] = (paddr >> 3) & 0x0FFFFFFFu;
    p_regs[0x23C] = 1u;
    ops = p_ext_submit_cmdlist(c, 0);
    p_regs[0x23C] = 0u;
    return ops;
}


/* Liest ein GPU-Register (Azahar VideoCore::GPU::ReadReg). */
uint32_t gpu_gsp_read_reg(ArmCpu *c, uint32_t addr)
{
    uint32_t page = addr & 0xFFFFF000u;
    (void)c;
    p_mmio_init();
    if (page == GPU_MMIO_LCD) {
        uint32_t i = (addr - GPU_MMIO_LCD) / 4u;
        return (i < GPU_MMIO_LCD_W) ? p_lcd_regs[i] : 0u;
    }
    if (page == GPU_MMIO_VADDR) {
        uint32_t i = (addr - GPU_MMIO_VADDR) / 4u;
        return (i < GPU_MMIO_EXT_W) ? p_ext_regs[i] : 0u;
    }
    if (page == GPU_MMIO_VADDR + 0x1000u) {
        uint32_t i = (addr - (GPU_MMIO_VADDR + 0x1000u)) / 4u;
        return (i < PICA_NREGS) ? p_regs[i] : 0u;
    }
    return 0u;
}

/* Schreibt ein GPU-Register mit Bytemaske (Azahar GPU::WriteReg bzw.
 * GSP::WriteHWRegsWithMask: reg = (reg & ~mask) | (value & mask)). */
void gpu_gsp_write_reg_masked(ArmCpu *c, uint32_t addr, uint32_t value,
                              uint32_t mask)
{
    uint32_t page = addr & 0xFFFFF000u;

    p_mmio_init();
    if (page == GPU_MMIO_LCD) {
        uint32_t i = (addr - GPU_MMIO_LCD) / 4u;
        if (i < GPU_MMIO_LCD_W)
            p_lcd_regs[i] = p_merge(p_lcd_regs[i], value, mask);
        return;
    }
    if (page == GPU_MMIO_VADDR) {
        uint32_t i = (addr - GPU_MMIO_VADDR) / 4u;
        if (i >= GPU_MMIO_EXT_W) return;
        p_ext_regs[i] = p_merge(p_ext_regs[i], value, mask);
        if (i == 0x07u) p_ext_memory_fill(c, 0, 0);
        else if (i == 0x0Bu) p_ext_memory_fill(c, 1, 1);
        else if (i == 0x306u) p_ext_transfer(c);
        return;
    }
    if (page == GPU_MMIO_VADDR + 0x1000u) {
        uint32_t i = (addr - (GPU_MMIO_VADDR + 0x1000u)) / 4u;
        if (i >= PICA_NREGS) return;
        p_regs[i] = p_merge(p_regs[i], value, mask);
        if (i >= 0x140u && i <= 0x1D9u) p_light_draw_invalidate();
        /* GPU::WriteReg behandelt nur den Kommandopuffer-Trigger als
         * Aktion; Draws/Finalize bleiben Kommandolisten vorbehalten.
         * GPU::SubmitCmdList prueft den Triggerwert und loescht ihn danach. */
        if (i == 0x23Cu) {
            if (p_regs[0x23C] != 0u) p_ext_submit_cmdlist(c, 0);
            p_regs[0x23C] = 0u;
        } else if (i == 0x23Du) {
            if (p_regs[0x23D] != 0u) p_ext_submit_cmdlist(c, 1);
            p_regs[0x23D] = 0u;
        }
        return;
    }
    /* Andere Seiten: Azahar UNREACHABLE, im Port ignorieren. */
}

void gpu_gsp_write_reg(ArmCpu *c, uint32_t addr, uint32_t value)
{
    gpu_gsp_write_reg_masked(c, addr, value, 0xFFFFFFFFu);
}

/* Azahar GPU::SetColorFill (GSP 0x000B SetLcdForceBlack): beide Screens
 * bekommen dieselbe ColorFill, Farbe 0 und is_enabled in Bit24
 * (Pica::ColorFill, regs_lcd.h). */
void gpu_gsp_set_lcd_color_fill(ArmCpu *c, int enable_black)
{
    uint32_t raw = enable_black ? 0x01000000u : 0u;   /* is_enabled = Bit24 */

    p_mmio_init();
    p_lcd_regs[0x81] = raw;    /* color_fill_top */
    p_lcd_regs[0x281] = raw;   /* color_fill_bottom */
    (void)c;
}

/* GX-Kommando 0x02 (Azahar GPU::Execute CommandId::MemoryFill): schreibt die
 * MemoryFill-Register und startet beide Fuellungen. Sind beide Puffer
 * gesetzt, meldet Puffer 0 keinen Interrupt und Puffer 1 PSC0. */
void gpu_gsp_memory_fill_cmd(ArmCpu *c, const uint32_t *w)
{
    uint32_t start1 = w[1], value1 = w[2], end1 = w[3];
    uint32_t start2 = w[4], value2 = w[5], end2 = w[6];
    uint32_t ctl1 = w[7] & 0xFFFFu, ctl2 = (w[7] >> 16) & 0xFFFFu;
    int both = (start1 != 0u) && (start2 != 0u);

    p_mmio_init();
    if (p_gspq_log())
        printf("[gpu] memfill s1=%08X e1=%08X v1=%08X c1=%X s2=%08X e2=%08X v2=%08X c2=%X\n",
               start1, end1, value1, ctl1, start2, end2, value2, ctl2);
    p_hw_tex_gen_bump();       /* MemoryFill kann Texturbytes ueberschreiben */
    if (start1 != 0u) {
        uint32_t ps = gpu_pica_virt_to_phys(start1);
        p_ext_regs[0x04] = ps >> 3;
        p_ext_regs[0x05] = (ps + (end1 - start1)) >> 3;
        p_ext_regs[0x06] = value1;
        p_ext_regs[0x07] = ctl1;
        p_ext_memory_fill(c, 0, both ? -1 : 0);
    }
    if (start2 != 0u) {
        uint32_t ps = gpu_pica_virt_to_phys(start2);
        p_ext_regs[0x08] = ps >> 3;
        p_ext_regs[0x09] = (ps + (end2 - start2)) >> 3;
        p_ext_regs[0x0A] = value2;
        p_ext_regs[0x0B] = ctl2;
        p_ext_memory_fill(c, 1, both ? 0 : 1);
    }
}

/* GX-Kommandos 0x03/0x04 (DisplayTransfer/TextureCopy): Register wie
 * GPU::Execute fuellen, dann denselben Trigger-Pfad wie MMIO-Schreiben. */
void gpu_gsp_display_transfer_cmd(ArmCpu *c, const uint32_t *w)
{
    uint32_t in_p = gpu_pica_virt_to_phys(w[1]);
    uint32_t out_p = gpu_pica_virt_to_phys(w[2]);

    p_mmio_init();
    p_ext_regs[0x300] = in_p >> 3;
    p_ext_regs[0x301] = out_p >> 3;
    p_ext_regs[0x303] = w[3];              /* input_size */
    p_ext_regs[0x302] = w[4];              /* output_size */
    p_ext_regs[0x304] = w[5];              /* flags */
    p_ext_regs[0x306] = 1u;                /* trigger */
    p_ext_transfer(c);
}

void gpu_gsp_texture_copy_cmd(ArmCpu *c, const uint32_t *w)
{
    uint32_t in_p = gpu_pica_virt_to_phys(w[1]);
    uint32_t out_p = gpu_pica_virt_to_phys(w[2]);

    p_mmio_init();
    p_ext_regs[0x300] = in_p >> 3;
    p_ext_regs[0x301] = out_p >> 3;
    /* Azahar regs_external.h: texture_copy beginnt bei Config-Offset 0x08
       (Register 0x308), nach trigger@0x306 und einem Pad-Wort@0x307. */
    p_ext_regs[0x308] = w[3];              /* texture_copy.size */
    p_ext_regs[0x309] = w[4];              /* input width/gap */
    p_ext_regs[0x30A] = w[5];              /* output width/gap */
    p_ext_regs[0x304] = w[6];              /* flags (Bit3 = texture copy) */
    p_ext_regs[0x306] = 1u;
    p_ext_transfer(c);
}

