#ifndef HW_PRESENT_H
#define HW_PRESENT_H

#include <stddef.h>
#include <stdint.h>

/* Bildausgabe des Hardware-Renderers (M17): DXGI-Swapchain je Fenster.
 *
 * Je Host-Frame baut hwp_frame einen Kanvas in interner Aufloesung
 * (Kanvasgroesse x hw_scale()) auf der GPU: Bildschirme, deren
 * Framebuffer aus einem GPU-DisplayTransfer stammen (hw_display_lookup),
 * kommen direkt aus der Anzeigeflaeche; alle anderen aus dem CPU-Bild,
 * das main.c ohnehin aus dem Gastspeicher dekodiert (identisch zum
 * Software-Pfad). hwp_present zeichnet einen Ausschnitt des Kanvas
 * seitenverhaeltnistreu in das Fenster.
 *
 * Reine Anzeige: liest Gastspeicher, schreibt ihn nie. Screenshots
 * (--shot) bleiben das native CPU-Bild; hwp_save_bmp schreibt zusaetzlich
 * den Kanvas in interner Aufloesung (Diagnose --shot-scaled). */

typedef struct {
    int valid;
    uint32_t address;   /* Gastadresse (Offset in mem) */
    uint32_t stride;    /* Bytes je Spalte, 0 = rows * bpp */
    uint32_t format;    /* GSP-Formatwort */
    int dx, dy;         /* Position im Kanvas (native Pixel) */
    int columns, rows;  /* Breite x Hoehe des Bildschirms */
} HwpScreen;

int hwp_init(int canvas_w, int canvas_h);   /* nach hw_init; 1 = aktiv */
int hwp_active(void);
void hwp_shutdown(void);

/* Kanvas fuer diesen Frame aufbauen; rgb = natives CPU-Bild (RGB24,
 * canvas_w x canvas_h), mem = Gastspeicher. */
void hwp_frame(const unsigned char *mem, size_t mem_size, const HwpScreen scr[2],
               const uint8_t *rgb);

/* Ausschnitt (src_*, native Kanvaspixel) in das Rechteck dst_* des
 * Fensters hwnd (Clientgroesse cw x ch) zeichnen, Rest schwarz, optional
 * Statuszeilen (GDI), dann Present. Rueckgabe 0 = nicht moeglich (der
 * Aufrufer zeichnet dann per GDI). */
int hwp_present(void *hwnd, int cw, int ch, int dst_x, int dst_y, int dst_w, int dst_h,
                int src_x, int src_y, int src_w, int src_h,
                const char *const *lines, int nlines);

/* Swapchain eines Fensters freigeben (vor DestroyWindow). */
void hwp_detach(void *hwnd);

/* Kanvas in interner Aufloesung als 24-Bit-BMP. */
int hwp_save_bmp(const char *path);

/* Diagnose: Bildschirme aus der GPU-Anzeigeflaeche / aus dem CPU-Bild. */
void hwp_stats(unsigned long long *gpu_screens, unsigned long long *cpu_screens);

#endif
