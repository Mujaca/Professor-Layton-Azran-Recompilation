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
 * 0x80=0 faellt p_texel auf Weiss zurueck. HLE_PICA_RENDER=1 setzt der
 * Runner (tools/test_pica_c.py).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

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
        mem[TEX + i * 3 + 0] = 255;
        mem[TEX + i * 3 + 1] = 0;
        mem[TEX + i * 3 + 2] = 0;
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
    p = emit(p, 0x22E, 0);                 /* Draw-Trigger (arrays) */
    size = (uint32_t)((unsigned char *)p - (mem + LIST));

    gpu_set_fb(FB);
    gpu_set_fb_size(FB, 400, 240);
    gpu_run_pica_list(&c, LIST, size);

    check(gpu_pica_texture_bound() == 1, "0x80 Bit0 schaltet Textur 0 ein");
    check(gpu_pica_list_count() == 1, "eine abgeschlossene Liste gezaehlt");
    check(gpu_pica_draw_count() == 1, "ein Draw gezaehlt");
    check(gpu_get_fb_width() == 400 && gpu_get_fb_height() == 240,
          "Framebuffer 400x240");

    cx = (FB + (120u * 400u + 200u) * 3u) & 0x1FFFFFFFu;
    check(mem[cx + 0] == 255 && mem[cx + 1] == 0 && mem[cx + 2] == 0,
          "Mitte rot (Textur ueber 0x200-Loader/Stride 16 gesampelt)");
    px = (FB + (0u * 400u + 0u) * 3u) & 0x1FFFFFFFu;
    check(mem[px + 0] == 0 && mem[px + 1] == 0 && mem[px + 2] == 0,
          "Ecke (0,0) ausserhalb des Dreiecks bleibt schwarz");

    /* Textur aus: p_texel faellt auf Weiss zurueck (Enable-Feld). */
    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x80, 0);
    p = emit(p, 0x22E, 0);
    size = (uint32_t)((unsigned char *)p - (mem + LIST));
    gpu_run_pica_list(&c, LIST, size);
    check(gpu_pica_texture_bound() == 0, "0x80=0 schaltet Textur 0 aus");
    check(mem[cx + 0] == 255 && mem[cx + 1] == 255 && mem[cx + 2] == 255,
          "Mitte weiss ohne Textur (Fallback-Pfad)");

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
        p = emit_h(p, 1u, 0x23Cu, 0xFu);                /* CMDBUF_JUMP0 */
        p = emit_h(p, 0x00000BADu, 0x241u, 0xFu);       /* darf nicht laufen */
        size = (uint32_t)((unsigned char *)p - (mem + LIST));
        ops = gpu_run_pica_list(&c, LIST, size);
        check(ops == 5, "Sprung zaehlt 4 aeussere + 1 innere Schreibung");
        check(gpu_pica_reg(0x24A) == 0x5A5A5A5Au, "Sprungliste wurde ausgefuehrt");
        check(gpu_pica_reg(0x23C) == 0x00000000u, "Trigger wird nach dem Lauf geloescht");
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

    printf("ERGEBNIS: %s\n", fails == 0 ? "OK" : "FEHLER");
    return fails == 0 ? 0 : 1;
}
