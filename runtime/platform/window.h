/* Minimales Win32-Fenster ohne externe Abhaengigkeiten. */
#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>

int win_open(int width, int height, const char *title);
void win_set_no_activate(int on);                 /* vor win_open: ohne Fokus zeigen */
void win_set_split(int on);   /* vor win_open: je Screen ein Fenster (ueberstimmt port.ini) */
/* Diagnose-Anzeige (Statuszeilen im Fenster, fps/Gast-Frames im Titel):
 * Standard aus; port.ini [debug] overlay = 1 oder --debug. win_set_debug
 * (Startoption) ueberstimmt port.ini; win_debug gilt nach win_open. */
void win_set_debug(int on);
int win_debug(void);
/* Nur Gast-fps oben links (Statuszeile 0); port.ini [debug] fps = 1 oder
 * --fps, --no-fps ueberstimmt. Mit Debug-Anzeige ist die Zeile ohnehin da. */
void win_set_fps_overlay(int on);
int win_pump(void);                              /* 1 = Fenster offen */
void win_blit(const uint8_t *rgb, int w, int h);  /* RGB24 -> Fenster */
void win_status(const char *text, int line);
void win_set_title(const char *text);            /* Titelzeile (fps/Frames) */
void win_close(void);

/* Bildbereich im Fenster: stufenloser Massstab (fuellt das Fenster so weit
 * wie moeglich), Seitenverhaeltnis canvas_w:canvas_h, zentriert (schwarze
 * Balken statt Verzerrung). Frueher ganzzahlig: ein Fenster knapp unter 2x
 * fiel auf 1x zurueck. Reine Funktion, damit tests/frame_clock_selftest.c
 * sie pruefen kann. */
typedef struct {
    int x, y, width, height;   /* Zielrechteck im Clientbereich */
    double scale;              /* Faktor > 0 (Fensterpixel je Kanvaspixel) */
} WinViewport;

static inline WinViewport win_fit_viewport(int client_w, int client_h,
                                           int canvas_w, int canvas_h) {
    WinViewport v;
    double sx, sy, s;
    if (canvas_w <= 0) canvas_w = 1;
    if (canvas_h <= 0) canvas_h = 1;
    if (client_w < 1) client_w = 1;
    if (client_h < 1) client_h = 1;
    sx = (double)client_w / canvas_w;
    sy = (double)client_h / canvas_h;
    s = (sx < sy) ? sx : sy;
    v.scale = s;
    v.width = (int)(canvas_w * s + 0.5);
    v.height = (int)(canvas_h * s + 0.5);
    if (v.width < 1) v.width = 1;
    if (v.height < 1) v.height = 1;
    v.x = (client_w - v.width) / 2;
    v.y = (client_h - v.height) / 2;
    return v;
}

/* Bildausgabe ueber eine andere API (M17: D3D11-Swapchain des
 * HW-Renderers, runtime/gpu/hw_present.c). Ist ein Hook gesetzt, ruft
 * WM_PAINT ihn je Fenster auf; gibt er 0 zurueck, zeichnet das Fenster wie
 * bisher per GDI aus dem Kanvas (win_blit). Viewport, Ausschnitt und
 * Touch-Umrechnung bleiben in window.c. */
typedef struct {
    void *hwnd;
    int pane;
    int client_w, client_h;
    WinViewport vp;                    /* Zielrechteck im Clientbereich */
    int src_x, src_y, src_w, src_h;    /* Kanvas-Ausschnitt (native Pixel) */
    const char *const *status;         /* Statuszeilen (nur Fenster 0) */
    int nstatus;
} WinPresentInfo;
typedef int (*WinPresentFn)(const WinPresentInfo *info);
typedef void (*WinDetachFn)(void *hwnd);
void win_set_present(WinPresentFn present, WinDetachFn detach);
/* [display] scale = N aus port.ini (0 = nicht gesetzt), nach win_open. */
int win_config_scale(void);

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
