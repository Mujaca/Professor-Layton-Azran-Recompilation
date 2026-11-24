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
    /* 3 Vertices, Stride 16: Position float3, UV float2. */
    set_vertex(&c, 0, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f);
    set_vertex(&c, 16, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f);
    set_vertex(&c, 32, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f);

    p = (uint32_t *)(mem + LIST);
    p = emit(p, 0x200, VTX >> 3);          /* (reg>>1)*16 = VTX */
    p = emit(p, 0x201, 0x7B);              /* attr0 float3, attr1 float2 */
    p = emit(p, 0x202, 1u << 28);          /* max_attribute_index = 1 */
    p = emit(p, 0x203, 0);                 /* Loader 0: data_offset 0 */
    p = emit(p, 0x204, 0x210);             /* comp0..2 = attr 0,1,2 */
    p = emit(p, 0x205, (3u << 28) | (16u << 16));  /* cc=3, stride=16 */
    p = emit(p, 0x206, 12);                /* Loader 1: UV bei +12 */
    p = emit(p, 0x207, 1);                 /* comp0 = attr 1 */
    p = emit(p, 0x208, (1u << 28) | (16u << 16));
    p = emit(p, 0x228, 3);                 /* num_vertices */
    p = emit(p, 0x80, 1);                  /* Textur an (Bit 0) */
    p = emit(p, 0x82, (2u << 16) | 2u);    /* 2x2 */
    p = emit(p, 0x85, TEX >> 3);
    p = emit(p, 0x8E, 1);                  /* RGB8 */
    p = emit(p, 0x22F, 0);                 /* Draw-Trigger */
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
    p = emit(p, 0x22F, 0);
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

    printf("ERGEBNIS: %s\n", fails == 0 ? "OK" : "FEHLER");
    return fails == 0 ? 0 : 1;
}
