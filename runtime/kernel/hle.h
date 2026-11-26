/* HLE-Schicht: SVCs, IPC und 3DS-Services fuer den recompilierten Code. */
#ifndef HLE_H
#define HLE_H

#include <stdint.h>

#include "arm_cpu.h"
#include "gsp_fb.h"
#include "hid_ring.h"

void hle_init(ArmCpu *c, const char *romfs_dir);
/* Host-Verzeichnis der Spielstaende (Standard "out/savedata"); vor hle_init
   aufrufen. Nur Host-Pfad, kein Gastzustand (main.c: --savedata). */
void hle_set_savedata_dir(const char *dir);
/* Der Gast-Lock gehoert dem Host-Thread, der Gastcode ausfuehrt (bei uns der
   game_thread in runtime/main.c), nicht dem Thread, der hle_init aufruft.
   Nur der Besitzer darf Gast-Bloecke ausfuehren bzw. den Lock kooperativ
   abgeben (hle_pump/guest_yield). */
void hle_guest_enter(void);
void hle_guest_leave(void);
uint32_t hle_svc(ArmCpu *c, uint32_t imm);
void hle_report(void);
void hle_tick_stats(int *calls, unsigned long long *tick);

/* Beim VBlank von der GSP uebernommener Framebuffer eines Screens
 * (0 = oben 400x240, 1 = unten 320x240). Adresse ist der aktive Puffer aus
 * der FrameBufferInfo (active_fb/shown_fb), stride in Bytes je Spalte,
 * format = GSP-Formatwort (Bits 0..2 Pixelformat, siehe docs/REFERENCE.md).
 * 3DS-Framebuffer sind spaltenweise: Pixel (x,y) liegt bei
 * address + x*stride + (239-y)*bpp. Liefert 1, wenn ein Puffer gesetzt ist.
 * Typ und VBlank-Latch stehen in runtime/platform/gsp_fb.h. */
typedef GspFbLatched HleGspFrame;

int hle_gsp_frame(int screen, HleGspFrame *out);

/* Anzahl der VBlanks, in denen der Gast eine neue FrameBufferInfo (also einen
 * eigenen Frame) veroeffentlicht hat. Nur eine Anzeige-Diagnose fuer die
 * Titelzeile; der Zaehler wird nie in Gastzustand zurueckgeschrieben. */
unsigned hle_gsp_guest_frames(void);

/* Wird vom GPU-Teil aufgerufen */
void hle_gpu_write_reg(uint32_t addr, uint32_t value);
void hle_gpu_command_list(const uint8_t *data, uint32_t size, uint32_t flags);

/* Host-Eingabe fuer hid:USER/hid:SPVR (runtime/main.c setzt sie je Frame aus
 * runtime/platform/window.c). Bitfelder: 1 = gedrueckt. Circle-Pad-Werte in
 * Rohkoordinaten -0x9A..0x9A (Azahar MAX_CIRCLEPAD_POS), Touch in Pixeln des
 * unteren Bildschirms (0..319 / 0..239). Layout in runtime/platform/
 * hid_ring.h (HidRingInput). */
typedef HidRingInput HleHidInput;

void hle_hid_set_input(const HleHidInput *in);
#endif
