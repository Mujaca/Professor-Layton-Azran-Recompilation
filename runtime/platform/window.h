/* Minimales Win32-Fenster ohne externe Abhaengigkeiten. */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

int win_open(int width, int height, const char *title);
int win_pump(void);                              /* 1 = Fenster offen */
void win_blit(const uint8_t *rgb, int w, int h);  /* RGB24 -> Fenster */
void win_status(const char *text, int line);
void win_set_title(const char *text);            /* Titelzeile (fps/Frames) */
void win_close(void);

/* Bildbereich im Fenster: ganzzahliger Massstab, Seitenverhaeltnis
 * canvas_w:canvas_h, zentriert (schwarze Balken statt Verzerrung). Reine
 * Funktion, damit tests/frame_clock_selftest.c sie pruefen kann. */
typedef struct {
    int x, y, width, height;   /* Zielrechteck im Clientbereich */
    int scale;                 /* ganzzahliger Faktor >= 1 */
} WinViewport;

static inline WinViewport win_fit_viewport(int client_w, int client_h,
                                           int canvas_w, int canvas_h) {
    WinViewport v;
    int sx, sy, s;
    if (canvas_w <= 0) canvas_w = 1;
    if (canvas_h <= 0) canvas_h = 1;
    sx = client_w / canvas_w;
    sy = client_h / canvas_h;
    s = (sx < sy) ? sx : sy;
    if (s < 1) s = 1;
    v.scale = s;
    v.width = canvas_w * s;
    v.height = canvas_h * s;
    v.x = (client_w - v.width) / 2;
    v.y = (client_h - v.height) / 2;
    return v;
}

/* Momentaufnahme der Host-Eingabe fuer hid. Standardbelegung (ueberschreibbar
 * in port.ini neben port.exe, siehe docs/FRONTEND.md):
 *   A=X, B=Z, X=S, Y=A, L=Q, R=W, Start=Enter, Select=Backspace,
 *   Steuerkreuz=Pfeiltasten, Circle-Pad=I/J/K/L (oben/links/unten/rechts).
 * Maus ueber dem Touch-Rechteck (win_set_touch_rect) = Touchscreen.
 * Ein XInput-Gamepad wird bei Bedarf zusaetzlich gelesen. */
typedef struct {
    int a, b, x, y, l, r, start, select;
    int up, down, left, right;
    int16_t circle_x, circle_y;      /* Rohwerte -0x9A..0x9A */
    int touch_pressed;
    int touch_x, touch_y;            /* Pixel im unteren Screen */
} WinInput;

void win_set_touch_rect(int x, int y, int w, int h);
void win_input_poll(WinInput *out);

int win_key_take(int vk);
void win_key_push(int vk);

#endif
