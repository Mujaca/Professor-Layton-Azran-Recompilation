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

/* GPU-MMIO-Registerfeld fuer gsp::Gpu WriteHWRegs/WriteHWRegsWithMask/
 * WriteHWRegRepeat/ReadHWRegs (Azahar VideoCore::GPU ReadReg/WriteReg).
 * addr ist die GPU-Virtualadresse (0x1EF00000-Bereich); Schreiben speichert
 * den Wert, Trigger-Bits starten MemoryFill/DisplayTransfer/Kommandoliste. */
void gpu_gsp_write_reg(ArmCpu *c, uint32_t addr, uint32_t value);
void gpu_gsp_write_reg_masked(ArmCpu *c, uint32_t addr, uint32_t value,
                              uint32_t mask);
uint32_t gpu_gsp_read_reg(ArmCpu *c, uint32_t addr);

/* GX-Kommandos 0x02..0x04 aus dem GSP-Shared-Memory-Kommandopuffer
 * (Azahar GPU::Execute): Register schreiben, Operation ausfuehren und die
 * Interrupts PSC0/PSC1/PPF ueber den IRQ-Handler melden. */
void gpu_gsp_memory_fill_cmd(ArmCpu *c, const uint32_t *w);
void gpu_gsp_display_transfer_cmd(ArmCpu *c, const uint32_t *w);
void gpu_gsp_texture_copy_cmd(ArmCpu *c, const uint32_t *w);

/* Vertex-Shader-Zustand (Tests/Diagnose): Float-Uniform idx/comp, ob ein
   Programm hochgeladen ist, und der zuletzt verarbeitete Vertex nach
   Ausgangs-Mapping (POSITION/COLOR/TEXCOORD0). */
float gpu_pica_vs_uniform(int index, int comp);
int gpu_pica_vs_active(void);
int gpu_pica_last_vertex(float *pos, float *color, float *uv);

/* Texturzustand fuer Textureinheit 0 aus dem Engine-Texturobjekt setzen. */
int gpu_pica_texture_bound(void);

/* Zaehler fuer abgeschlossene PICA-Kommandolisten (Engine-Submits) und
   tatsaechlich ausgefuehrte Draws. Der Screenshot wartet darauf, statt
   sich auf einen Frame-Zeitpunkt zu verlassen (Determinismus). */
unsigned long long gpu_pica_list_count(void);
unsigned long long gpu_pica_draw_count(void);

#endif
