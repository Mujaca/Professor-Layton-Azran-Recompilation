#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include "arm_cpu.h"

void gpu_set_fb(uint32_t addr);
uint32_t gpu_get_fb(void);
void gpu_set_fb_size(uint32_t addr, int w, int h);
int gpu_get_fb_width(void);
int gpu_get_fb_height(void);

/* Fuehrt eine (Teil-)PICA-Kommandoliste (altes HLE-Listenformat) aus.
   Rueckgabe: Anzahl ausgefuehrter Zeichenoperationen. */

/* Echte PICA200-Kommandoliste der Engine: Paare (value, header) mit
   cmd_id/mask/extra/group wie in Azahar pica_core.cpp. */
int gpu_run_pica_list(ArmCpu *c, uint32_t addr, uint32_t size);
void gpu_pica_reset(void);

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
