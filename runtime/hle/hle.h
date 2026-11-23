/* HLE-Schicht: SVCs, IPC und 3DS-Services fuer den recompilierten Code. */
#ifndef HLE_H
#define HLE_H

#include <stdint.h>

#include "../arm_cpu.h"

void hle_init(ArmCpu *c, const char *romfs_dir荟);
uint32_t hle_svc(ArmCpu *c, uint32_t imm);
void hle_report(void);
void hle_tick_stats(int *calls, unsigned long long *tick);

/* Framebuffer-Adresse je Screen (0 = oben, 1 = unten), gesetzt durch
   gsp::Gpu SetBufferSwap (0x0005). */
uint32_t hle_gsp_fb_addr(int screen);

/* Wird vom GPU-Teil aufgerufen */
void hle_gpu_write_reg(uint32_t addr, uint32_t value);
void hle_gpu_command_list(const uint8_t *data, uint32_t size, uint32_t flags);

#endif
