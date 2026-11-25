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

/* Renderer-Auswahl (nur per Kommandozeile --renderer hw): 1 = Rasterisierung
 * und Fragment-Pipeline auf D3D11, 0 = Software-Rasterizer (Standard). */
int gpu_pica_set_render_hw(int on);
int gpu_pica_render_hw(void);

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
/* GSP 0x000B SetLcdForceBlack -> Azahar GPU::SetColorFill:
 * color_fill_top/bottom beider Screens (is_enabled = Bit24). */
void gpu_gsp_set_lcd_color_fill(ArmCpu *c, int enable_black);

/* GX-Kommandos 0x01..0x04 aus dem GSP-Shared-Memory-Kommandopuffer
 * (Azahar GPU::Execute): Register schreiben, Operation ausfuehren und die
 * Interrupts PSC0/PSC1/PPF/P3D ueber den IRQ-Handler melden. */
int gpu_gsp_submit_cmdlist_cmd(ArmCpu *c, uint32_t paddr, uint32_t size);
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

/* Gastspeicher wurde ausserhalb der PICA-Liste geschrieben (Y2R,
 * DisplayTransfer, TextureCopy, MemoryFill): Hardware-Ziel neu laden. */
void gpu_pica_invalidate_guest(void);

/* Software-Texeldekoder (alle PICA-Formate inkl. ETC1/ETC1A4) fuer den
   Hardware-Pfad: Groesse einer 8x8-Kachel in Bytes und ein Texel einer
   Kachel (x,y = 0..7 innerhalb der Kachel, Ausgabe RGBA8). */
uint32_t gpu_pica_tex_tile_size(uint32_t fmt);
void gpu_pica_decode_texel(const uint8_t *tile, uint32_t fmt, uint32_t x,
                           uint32_t y, uint8_t out[4]);

/* Generation des Gast-Texturspeichers: erhoeht sich bei jeder neuen
   Kommandoliste und bei MemoryFill/DisplayTransfer/TextureCopy. Ein
   Textur-Cache darf seinen Inhalt nur bei gleicher Generation weiterverwenden. */
uint32_t gpu_pica_tex_generation(void);

/* Zaehler fuer abgeschlossene PICA-Kommandolisten (Engine-Submits) und
   tatsaechlich ausgefuehrte Draws. Der Screenshot wartet darauf, statt
   sich auf einen Frame-Zeitpunkt zu verlassen (Determinismus). */
unsigned long long gpu_pica_list_count(void);
unsigned long long gpu_pica_draw_count(void);
/* Diagnose (HLE_GTIME): Arbeitsvolumen des Software-Rasterizers. */
void gpu_pica_work_stats(unsigned long long *frag, unsigned long long *tex,
                         unsigned long long *lit, unsigned long long *fb);

/* Paralleles Rasterisieren grosser Dreiecke: n Worker-Threads anlegen bzw.
 * abschalten (0 = seriell, fuer Vergleichsmessungen). Das Ergebnis ist
 * pixelgleich; nur die Laufzeit aendert sich. */
void gpu_pica_set_raster_threads(int n);
/* Diagnose: Anzahl paralleler/serieller Dreiecke und Pixelzeilen. */
void gpu_pica_par_stats(unsigned long long *pc, unsigned long long *sc,
                        unsigned long long *pr, unsigned long long *sr);

#endif
