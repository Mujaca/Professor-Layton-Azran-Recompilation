/* GSP-Framebuffer-Register nach Azahar.
 *
 * Referenz: tools/azahar/src/video_core/gpu.cpp (GPU::SetBufferSwap),
 * tools/azahar/src/video_core/pica/regs_external.h (FramebufferConfig),
 * tools/azahar/src/core/hle/service/gsp/gsp_gpu.h (FrameBufferInfo).
 *
 * SetBufferSwap schreibt nur Registerenzustand: address_left1/2 werden nach
 * info.active_fb getrennt gefuellt, active_fb uebernimmt info.shown_fb, dazu
 * stride und die unteren 16 Bit des Formatworts. Was die Anzeige liest, ist
 * address_left1 bzw. address_left2 je nach active_fb. Wirksam wird der Wechsel
 * erst beim naechsten VBlank, wenn die Anzeige den Stand uebernimmt.
 *
 * Diese Datei ist reine Zustandslogik ohne Runtime-Abhaengigkeiten, damit
 * tests/framebuffer_selftest.c sie ohne HLE/CPU pruefen kann.
 */
#ifndef GSP_FB_H
#define GSP_FB_H

#include <stdint.h>

/* GSP-FrameBufferInfo (3dbrew "Framebuffer Info"):
 *   +0x00 active_fb   (0 = ...1, 1 = ...2)
 *   +0x04 address_left
 *   +0x08 address_right
 *   +0x0C stride       (Bytes zwischen zwei Spalten)
 *   +0x10 format       (Bits 0..2 Pixelformat, 0x70/0x20 = 3D oben)
 *   +0x14 shown_fb     (wird GPU-Register active_fb)
 */
#define GSP_FB_FORMAT_PIXEL_MASK  0x7u
#define GSP_FB_FORMAT_STEREO_MASK 0x70u
#define GSP_FB_FORMAT_STEREO_3D   0x20u

typedef struct {
    uint32_t address_left1;   /* Puffer "...1" (info.active_fb == 0) */
    uint32_t address_left2;   /* Puffer "...2" (info.active_fb == 1) */
    uint32_t address_right1;  /* rechtes Auge, nur oben/3D */
    uint32_t address_right2;
    uint32_t stride;          /* Bytes zwischen zwei Spalten */
    uint32_t format;          /* untere 16 Bit des GSP-Formatworts */
    uint32_t active_fb;       /* 0 = ...1, 1 = ...2 (GPU-Register) */
} GspFbRegs;

/* Startwerte wie Azahar PicaCore::InitializeRegs (nn::gx::Initialize),
 * nur mit den VRAM-Adressen dieses Ports (Azahar nutzt 0x181Exxxx). */
static inline void gsp_fb_regs_reset(GspFbRegs *r, uint32_t left1, uint32_t left2,
                                     uint32_t stride, uint32_t format) {
    r->address_left1 = left1;
    r->address_left2 = left2;
    r->address_right1 = left1;
    r->address_right2 = left2;
    r->stride = stride;
    r->format = format & 0xFFFFu;
    r->active_fb = 0;
}

/* Wendet eine FrameBufferInfo auf die Register an (GPU::SetBufferSwap).
 * screen 0 = oben: im 3D-Modus ((format & 0x70) == 0x20) und bei
 * address_right != 0 wird das rechte Auge getrennt gefuehrt, sonst zeigt es
 * auf denselben Puffer wie links. screen 1 (unten) hat kein rechtes Auge.
 * Eine Adresse 0 bedeutet "nicht gesetzt" und wird ignoriert. */
static inline void gsp_fb_regs_apply(GspFbRegs *r, int screen,
                                     uint32_t info_active_fb,
                                     uint32_t address_left, uint32_t address_right,
                                     uint32_t stride, uint32_t format,
                                     uint32_t shown_fb) {
    if (address_left == 0u) {
        return;
    }
    if (screen == 0) {
        uint32_t right = address_left;
        if ((format & GSP_FB_FORMAT_STEREO_MASK) == GSP_FB_FORMAT_STEREO_3D &&
            address_right != 0u) {
            right = address_right;
        }
        if (info_active_fb == 0u) {
            r->address_right1 = right;
        } else {
            r->address_right2 = right;
        }
    }
    if (info_active_fb == 0u) {
        r->address_left1 = address_left;
    } else {
        r->address_left2 = address_left;
    }
    r->stride = stride;
    r->format = format & 0xFFFFu;
    r->active_fb = shown_fb & 0xFFu;
}

static inline uint32_t gsp_fb_regs_visible_left(const GspFbRegs *r) {
    return (r->active_fb == 0u) ? r->address_left1 : r->address_left2;
}

static inline uint32_t gsp_fb_regs_visible_right(const GspFbRegs *r) {
    return (r->active_fb == 0u) ? r->address_right1 : r->address_right2;
}

/* Beim VBlank von der Anzeige uebernommener Stand. Die Anzeige liest ihn,
 * waehrend das Spiel weitere SetBufferSwap-Aufrufe macht; dadurch wechselt
 * das Bild genau im VBlank. */
typedef struct {
    int valid;          /* 0 = kein Puffer gesetzt */
    uint32_t address;   /* sichtbarer Puffer (active_fb) */
    uint32_t stride;    /* Bytes zwischen zwei Spalten */
    uint32_t format;    /* GSP-Formatwort, Bits 0..2 Pixelformat */
} GspFbLatched;

static inline void gsp_fb_latch(const GspFbRegs *r, GspFbLatched *out) {
    uint32_t addr = gsp_fb_regs_visible_left(r);
    if (addr == 0u) {
        out->valid = 0;
        return;
    }
    out->valid = 1;
    out->address = addr;
    out->stride = r->stride;
    out->format = r->format;
}

#endif
