/* Minimales Win32-Fenster ohne externe Abhaengigkeiten. */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

int win_open(int width, int height, const char *title);
int win_pump(void);                              /* 1 = Fenster offen */
void win_blit(const uint8_t *rgb, int w, int h);  /* RGB24 -> Fenster */
void win_status(const char *text, int line);
void win_close(void);

/* Momentaufnahme der Host-Eingabe fuer hid. Tastenbelegung:
 *   A=X, B=Z, X=S, Y=A, L=Q, R=W, Start=Enter, Select=Backspace,
 *   Steuerkreuz=Pfeiltasten, Circle-Pad=I/J/K/L (oben/links/unten/rechts).
 * Maus ueber dem Touch-Rechteck (win_set_touch_rect) = Touchscreen. */
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
