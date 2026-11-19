#ifndef GPU_H
#define GPU_H

#include <stdint.h>
#include "arm_cpu.h"

void gpu_set_fb(uint32_t addr);
uint32_t gpu_get_fb(void);

/* Fuehrt eine (Teil-)PICA-Kommandoliste aus und schreibt Pixel in den
   Framebuffer. Rueckgabe: Anzahl ausgefuehrter Zeichenoperationen. */
int gpu_run_command_list(ArmCpu *c, uint32_t addr, uint32_t size);

#endif
