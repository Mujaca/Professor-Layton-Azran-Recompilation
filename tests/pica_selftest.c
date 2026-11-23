/* PICA-Registermap-Test (ohne Gast-Code).
 *
 * Baut eine synthetische PICA-Kommandoliste und prueft:
 *   0x200  Vertex-Basis (base = (reg>>1)*16)
 *   0x201  Attributformate (attr0 float3, attr1 float2)
 *   0x203+ Loader (byte_count = Stride 16 im Wort 2)
 *   0x228  num_vertices, 0x22F Draw-Trigger
 *   0x80   Textur-Enable ist Bit 0 von main_config
 *   0x82/0x85/0x8E Groesse/Adresse/Format
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

static int fails = 0;

static void check(int cond, const char *what) {
    printf("%s %s\n", cond ? "OK  " : "FEHL", what);
    if (!cond) fails++;
}

static void wr32(ArmCpu *c, uint32_t a, uint32_t v) {
    memcpy(c->mem + (a & 0x1FFFFFFFu), &v, 4);
}

static uint32_t *emit(uint32_t *p, uint32_t id, uint32_t val) {
    p[0] = val;
    p[1] = id | (0xFu << 16);
    return p + 2;
}

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

    printf("ERGEBNIS: %s\n", fails == 0 ? "OK" : "FEHLER");
    return fails == 0 ? 0 : 1;
}
