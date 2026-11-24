#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include "arm_cpu.h"

void gpu_set_fb(uint32_t addr);
uint32_t gpu_get_fb(void);
void gpu_set_fb_size(uint32_t addr, int w, int h);
int gpu_get_fb_width(void);
int gpu_get_fb_height(void);

/* Echte PICA200-Kommandoliste der Engine: Paare (value, header) mit
   cmd_id/mask/extra/group wie in Azahar pica_core.cpp. addr ist eine
   physische PICA-Adresse (GSP-Submits vorher mit gpu_pica_virt_to_phys
   umrechnen); Rueckgabe = Anzahl verarbeiteter Register-Schreibungen. */
int gpu_run_pica_list(ArmCpu *c, uint32_t addr, uint32_t size);
void gpu_pica_reset(void);

/* Physisch -> Gast-Linearspeicher (Azahar MemorySystem):
   VRAM 0x18000000..0x185FFFFF -> 0x1F000000.., FCRAM 0x20000000.. ->
   Linear-Heap 0x14000000... Bereits virtuelle/lineare Adressen bleiben
   unveraendert (der Port hat keine MMU/Seitentabelle). */
uint32_t gpu_pica_phys_to_guest(uint32_t paddr);
/* Umkehrung fuer GSP-Submits: lineare Heap-/VRAM-Virtualadresse -> physisch. */
uint32_t gpu_pica_virt_to_phys(uint32_t vaddr);

/* P3D-Interrupt aus dem Kommandostrom (Register 0x010 Finalize). */
void gpu_pica_set_irq_handler(void (*fn)(int id));

/* Nur fuer Diagnose/Tests: zuletzt geschriebener Registerinhalt. */
uint32_t gpu_pica_reg(uint32_t id);

/* Vertex-Shader-Zustand (Tests/Diagnose): Float-Uniform idx/comp, ob ein
   Programm hochgeladen ist, und der zuletzt verarbeitete Vertex nach
   Ausgangs-Mapping (POSITION/COLOR/TEXCOORD0). */
float gpu_pica_vs_uniform(int index, int comp);
int gpu_pica_vs_active(void);
int gpu_pica_last_vertex(float *pos, float *color, float *uv);

/* Texturzustand fuer Textureinheit 0 aus dem Engine-Texturobjekt setzen.
   Register: 0x80 main_config (bit0 enable), 0x82 height[0..10]/width[16..26],
   0x85 Adresse/8, 0x8E Format (1 = RGB8). */
void gpu_pica_bind_texture(uint32_t addr, int w, int h, int fmt);
int gpu_pica_texture_bound(void);
void gpu_pica_set_uv(float u0, float v0, float u1, float v1);
void gpu_pica_get_uv(float *u0, float *v0, float *u1, float *v1);

/* Zaehler fuer abgeschlossene PICA-Kommandolisten (Engine-Submits) und
   tatsaechlich ausgefuehrte Draws. Der Screenshot wartet darauf, statt
   sich auf einen Frame-Zeitpunkt zu verlassen (Determinismus). */
unsigned long long gpu_pica_list_count(void);
unsigned long long gpu_pica_draw_count(void);

#endif
