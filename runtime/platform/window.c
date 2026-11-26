#include "window.h"

#include <windows.h>
#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *g_dib = NULL;
static int g_w = 400, g_h = 240;
static char g_status[10][192];
static int g_status_lines = 0;
static int g_keys[256];
/* Tastatur-/Mauszustand fuer HID. Der WndProc-Hostthread schreibt, der
 * Port-Hauptthread liest; SRWLOCK schuetzt die Momentaufnahme. */
static SRWLOCK g_in_srw = SRWLOCK_INIT;
static int g_key_down[256];
static int g_mouse_x = -1, g_mouse_y = -1;
static int g_mouse_down = 0;
static int g_touch_x = 0, g_touch_y = 240, g_touch_w = 320, g_touch_h = 240;
static int g_has_touch_rect = 0;
static int g_xinput_enabled = 1;
static int g_layout_ini = 0;          /* [display] layout = split -> 1 */
static int g_scale_ini = 0;           /* [display] scale = N (0 = nicht gesetzt) */
static int g_debug_ini = 0;           /* [debug] overlay = 1 */
static int g_debug_cli = -1;          /* --debug/--no-debug, -1 = port.ini */
static int g_fps_ini = 0;             /* [debug] fps = 1 */
static int g_fps_cli = -1;            /* --fps/--no-fps, -1 = port.ini */

static int win_fps_overlay(void) {
    return (g_fps_cli >= 0) ? g_fps_cli : g_fps_ini;
}

void win_set_fps_overlay(int on) {
    g_fps_cli = on != 0;
}

/* Zeilen fuer das Overlay (nur erstes Fenster): mit Debug alle
 * Statuszeilen, sonst bei FPS-Anzeige nur Zeile 0 (Gast-fps). */
int win_debug(void);
static int overlay_lines(const void *pane, const char *out[10]);
static WinPresentFn g_present_fn = NULL;
static WinDetachFn g_detach_fn = NULL;

/* ---- Tastenbelegung (port.ini neben port.exe, siehe docs/FRONTEND.md) ---- */
typedef struct {
    int a, b, x, y, l, r, start, select;
    int up, down, left, right;
    int circle_up, circle_down, circle_left, circle_right;
} KeyMap;

static KeyMap g_map = {
    'X', 'Z', 'S', 'A', 'Q', 'W', VK_RETURN, VK_BACK,
    VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT,
    'I', 'K', 'J', 'L'
};

static void config_path(char *out, size_t n) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    if (len > 0 && len < sizeof(exe)) {
        char *slash = strrchr(exe, '\\');
        if (slash != NULL) {
            *slash = '\0';
            snprintf(out, n, "%s\\port.ini", exe);
            return;
        }
    }
    snprintf(out, n, "port.ini");
}

static void write_default_config(const char *path) {
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fputs("# port.ini - Host-Tastenbelegung fuer Professor Layton (3DS-Port)\n"
          "# Tasten: Buchstabe/Ziffer oder Enter, Backspace, Tab, Escape, Space,\n"
          "# Up, Down, Left, Right, Home, End, PageUp, PageDown, Insert, Delete,\n"
          "# Shift, Ctrl, Alt, F1..F12; NONE loest die Taste; 0x<hex> = Win32-VK.\n"
          "# Standardbelegung:\n"
          "[keys]\n"
          "A = X\n"
          "B = Z\n"
          "X = S\n"
          "Y = A\n"
          "L = Q\n"
          "R = W\n"
          "START = Enter\n"
          "SELECT = Backspace\n"
          "UP = Up\n"
          "DOWN = Down\n"
          "LEFT = Left\n"
          "RIGHT = Right\n"
          "CIRCLE_UP = I\n"
          "CIRCLE_DOWN = K\n"
          "CIRCLE_LEFT = J\n"
          "CIRCLE_RIGHT = L\n"
          "\n"
          "[gamepad]\n"
          "# XInput-Gamepad zusaetzlich zur Tastatur (0 = aus).\n"
          "enabled = 1\n"
          "\n"
          "[display]\n"
          "# single = ein Fenster mit beiden Screens, split = je Screen ein\n"
          "# Fenster (auch per Startoption --split-screens).\n"
          "layout = single\n"
          "# Interne Aufloesung des Hardware-Renderers (1..4, auch --scale N).\n"
          "scale = 1\n"
          "\n"
          "[debug]\n"
          "# 1 = Diagnosezeilen im Fenster und fps/Gast-Frames in der Titelzeile\n"
          "# (auch per Startoption --debug / --no-debug).\n"
          "overlay = 0\n"
          "# 1 = nur die Gast-fps (Bildrate des Spiels) oben links im Fenster\n"
          "# (auch per Startoption --fps / --no-fps).\n"
          "fps = 0\n",
          f);
    fclose(f);
}

static int ieq(const char *a, const char *b) {
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

static char *trim(char *s) {
    char *e;
    while (*s == ' ' || *s == '\t') s++;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' ||
                     e[-1] == '\n')) {
        *--e = '\0';
    }
    return s;
}

/* -1 = unbekannt (Standard bleibt), 0 = NONE, sonst Win32-VK. */
static int key_name_to_vk(const char *s) {
    static const struct { const char *name; int vk; } named[] = {
        {"ENTER", VK_RETURN}, {"RETURN", VK_RETURN}, {"BACKSPACE", VK_BACK},
        {"TAB", VK_TAB}, {"ESCAPE", VK_ESCAPE}, {"ESC", VK_ESCAPE},
        {"SPACE", VK_SPACE}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
        {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"HOME", VK_HOME},
        {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
        {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE}, {"DEL", VK_DELETE},
        {"SHIFT", VK_SHIFT}, {"CTRL", VK_CONTROL}, {"CONTROL", VK_CONTROL},
        {"ALT", VK_MENU}, {"NONE", 0},
        {"F1", VK_F1}, {"F2", VK_F2}, {"F3", VK_F3}, {"F4", VK_F4},
        {"F5", VK_F5}, {"F6", VK_F6}, {"F7", VK_F7}, {"F8", VK_F8},
        {"F9", VK_F9}, {"F10", VK_F10}, {"F11", VK_F11}, {"F12", VK_F12},
    };
    size_t i;
    if (s == NULL || *s == '\0') return -1;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        char *end = NULL;
        long v = strtol(s + 2, &end, 16);
        if (end != NULL && *end == '\0' && v >= 0 && v <= 255) return (int)v;
        return -1;
    }
    for (i = 0; i < sizeof(named) / sizeof(named[0]); i++) {
        if (ieq(s, named[i].name)) return named[i].vk;
    }
    if (strlen(s) == 1 && isalnum((unsigned char)s[0])) {
        return (int)toupper((unsigned char)s[0]);
    }
    return -1;
}

static int *map_slot(KeyMap *m, const char *name) {
    static const struct { const char *name; size_t off; } slots[] = {
        {"A", offsetof(KeyMap, a)}, {"B", offsetof(KeyMap, b)},
        {"X", offsetof(KeyMap, x)}, {"Y", offsetof(KeyMap, y)},
        {"L", offsetof(KeyMap, l)}, {"R", offsetof(KeyMap, r)},
        {"START", offsetof(KeyMap, start)}, {"SELECT", offsetof(KeyMap, select)},
        {"UP", offsetof(KeyMap, up)}, {"DOWN", offsetof(KeyMap, down)},
        {"LEFT", offsetof(KeyMap, left)}, {"RIGHT", offsetof(KeyMap, right)},
        {"CIRCLE_UP", offsetof(KeyMap, circle_up)},
        {"CIRCLE_DOWN", offsetof(KeyMap, circle_down)},
        {"CIRCLE_LEFT", offsetof(KeyMap, circle_left)},
        {"CIRCLE_RIGHT", offsetof(KeyMap, circle_right)},
    };
    size_t i;
    for (i = 0; i < sizeof(slots) / sizeof(slots[0]); i++) {
        if (ieq(name, slots[i].name)) {
            return (int *)((char *)m + slots[i].off);
        }
    }
    return NULL;
}

static void load_config(void) {
    char path[MAX_PATH + 32];
    char line[512];
    char section[32] = "";
    FILE *f;
    config_path(path, sizeof(path));
    f = fopen(path, "rb");
    if (f == NULL) {
        write_default_config(path);
        f = fopen(path, "rb");
        if (f == NULL) return;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char *s = trim(line);
        char *eq;
        if (*s == '\0' || *s == '#' || *s == ';') continue;
        if (*s == '[') {
            char *close = strchr(s, ']');
            if (close != NULL) {
                *close = '\0';
                snprintf(section, sizeof(section), "%s", trim(s + 1));
            }
            continue;
        }
        eq = strchr(s, '=');
        if (eq == NULL) continue;
        *eq = '\0';
        {
            char *key = trim(s);
            char *val = trim(eq + 1);
            if (ieq(section, "keys")) {
                int *slot = map_slot(&g_map, key);
                int vk = key_name_to_vk(val);
                if (slot != NULL && vk >= 0) *slot = vk;
            } else if (ieq(section, "gamepad") && ieq(key, "enabled")) {
                g_xinput_enabled = (atoi(val) != 0);
            } else if (ieq(section, "display") && ieq(key, "layout")) {
                if (ieq(val, "split")) g_layout_ini = 1;
                else if (ieq(val, "single")) g_layout_ini = 0;
            } else if (ieq(section, "debug") && ieq(key, "overlay")) {
                g_debug_ini = (atoi(val) != 0);
            } else if (ieq(section, "debug") && ieq(key, "fps")) {
                g_fps_ini = (atoi(val) != 0);
            } else if (ieq(section, "display") && ieq(key, "scale")) {
                int v = atoi(val);
                if (v >= 1 && v <= 4) g_scale_ini = v;
            }
        }
    }
    fclose(f);
}

/* ---- XInput (optional, per LoadLibrary; kein harter Link) ---- */
typedef struct {
    WORD wButtons;
    BYTE bLeftTrigger;
    BYTE bRightTrigger;
    SHORT sThumbLX;
    SHORT sThumbLY;
    SHORT sThumbRX;
    SHORT sThumbRY;
} XInGamepad;

typedef struct {
    DWORD dwPacketNumber;
    XInGamepad Gamepad;
} XInState;

typedef DWORD(WINAPI *XInputGetStateFn)(DWORD, XInState *);

static XInputGetStateFn g_xinput_get = NULL;
static int g_xinput_tried = 0;

static void xinput_init(void) {
    static const char *dlls[] = {"xinput1_4.dll", "xinput1_3.dll",
                                 "xinput9_1_0.dll"};
    size_t i;
    if (g_xinput_tried) return;
    g_xinput_tried = 1;
    for (i = 0; i < sizeof(dlls) / sizeof(dlls[0]); i++) {
        HMODULE h = LoadLibraryA(dlls[i]);
        if (h != NULL) {
            g_xinput_get = (XInputGetStateFn)(void *)GetProcAddress(
                h, "XInputGetState");
            if (g_xinput_get != NULL) return;
        }
    }
}

/* Fuellt out mit dem Gamepad-Zustand; Rueckgabe 1 = verbunden. Tasten werden
 * ODER-verknuepft, der linke Stick ersetzt das Tastatur-Circle-Pad, sobald er
 * die XInput-Deadzone (7849) verlaesst. */
static int xinput_poll(WinInput *out) {
    int idx;
    if (!g_xinput_enabled || g_xinput_get == NULL) return 0;
    for (idx = 0; idx < 4; idx++) {
        XInState st;
        memset(&st, 0, sizeof(st));
        if (g_xinput_get((DWORD)idx, &st) == 0) {
            WORD b = st.Gamepad.wButtons;
            if (b & 0x1000) out->a = 1;
            if (b & 0x2000) out->b = 1;
            if (b & 0x4000) out->x = 1;
            if (b & 0x8000) out->y = 1;
            if (b & 0x0100) out->l = 1;
            if (b & 0x0200) out->r = 1;
            if (b & 0x0010) out->start = 1;
            if (b & 0x0020) out->select = 1;
            if (b & 0x0001) out->up = 1;
            if (b & 0x0002) out->down = 1;
            if (b & 0x0004) out->left = 1;
            if (b & 0x0008) out->right = 1;
            {
                long lx = st.Gamepad.sThumbLX;
                long ly = st.Gamepad.sThumbLY;
                if (lx * lx + ly * ly > 7849L * 7849L) {
                    if (lx > 32767) lx = 32767;
                    if (lx < -32767) lx = -32767;
                    if (ly > 32767) ly = 32767;
                    if (ly < -32767) ly = -32767;
                    out->circle_x = (int16_t)lround((double)lx * 0x9A / 32767.0);
                    out->circle_y = (int16_t)lround((double)ly * 0x9A / 32767.0);
                }
            }
            return 1;
        }
    }
    return 0;
}

/* ---- Fenster ----
 * Ein "Pane" ist ein Fenster, das einen Ausschnitt des Kanvas zeigt.
 * Standard: ein Fenster mit dem ganzen Kanvas (beide Screens untereinander).
 * Layout "split" (--split-screens oder [display] layout = split): zwei
 * Fenster, oben der obere Screen, unten der Touchscreen (Ausschnitt =
 * Touch-Rechteck). Jedes Fenster skaliert und schaltet Vollbild (F11) fuer
 * sich; Tastatur/Gamepad wirken, solange eines der Fenster vorne ist. Die
 * Fensterschicht aendert nur die Anzeige, nie den Gastzustand. */
typedef struct {
    HWND hwnd;
    int src_x, src_y, src_w, src_h;   /* Ausschnitt des Kanvas */
    WinViewport vp;
    BITMAPINFO bi;                    /* Hoehe = src_h (top-down) */
    HDC back_dc;                      /* Doppelpufferung, siehe draw() */
    HBITMAP back_bmp, back_old;
    int back_w, back_h;
    int fullscreen;
    LONG windowed_style;
    RECT windowed_rect;
} WinPane;

static WinPane g_pane[2];
static int g_npanes = 0;
static int g_layout_cli = -1;         /* -1 = port.ini entscheidet */

void win_set_split(int on) {
    g_layout_cli = on != 0;
}

static WinPane *pane_of(HWND hwnd) {
    int i;
    for (i = 0; i < g_npanes; i++)
        if (g_pane[i].hwnd == hwnd) return &g_pane[i];
    return NULL;
}

static void pane_set_src(WinPane *p, int x, int y, int w, int h) {
    p->src_x = x;
    p->src_y = y;
    p->src_w = w;
    p->src_h = h;
    memset(&p->bi, 0, sizeof(p->bi));
    p->bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    p->bi.bmiHeader.biWidth = g_w;    /* Zeilenlaenge des ganzen Kanvas */
    p->bi.bmiHeader.biHeight = -h;    /* top-down, ab Zeile src_y */
    p->bi.bmiHeader.biPlanes = 1;
    p->bi.bmiHeader.biBitCount = 32;
    p->bi.bmiHeader.biCompression = BI_RGB;
}

static void update_viewport(WinPane *p) {
    RECT rc;
    if (p == NULL || p->hwnd == NULL) return;
    GetClientRect(p->hwnd, &rc);
    p->vp = win_fit_viewport((int)rc.right, (int)rc.bottom, p->src_w, p->src_h);
}

static void toggle_fullscreen(WinPane *p) {
    HWND hwnd = p->hwnd;
    if (!p->fullscreen) {
        MONITORINFO mi;
        HMONITOR mon;
        p->windowed_style = GetWindowLongA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &p->windowed_rect);
        mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
        memset(&mi, 0, sizeof(mi));
        mi.cbSize = sizeof(mi);
        if (mon == NULL || !GetMonitorInfoA(mon, &mi)) return;
        SetWindowLongA(hwnd, GWL_STYLE,
                       (p->windowed_style & ~(WS_CAPTION | WS_THICKFRAME |
                                              WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                                              WS_SYSMENU)) |
                           WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        p->fullscreen = 1;
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, p->windowed_style);
        SetWindowPos(hwnd, HWND_TOP, p->windowed_rect.left,
                     p->windowed_rect.top,
                     p->windowed_rect.right - p->windowed_rect.left,
                     p->windowed_rect.bottom - p->windowed_rect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        p->fullscreen = 0;
    }
    update_viewport(p);
}

/* Hintergrundpuffer (Doppelpufferung). Frueher wurde direkt ins Fenster
 * gezeichnet: erst der ganze Client-Bereich schwarz (FillRect), dann das Bild
 * (StretchDIBits), dann der Statustext - 60-mal pro Sekunde. Der kurz
 * sichtbare schwarze/teilgezeichnete Zwischenstand war das Flackern im
 * Fenster (M13); Screenshots zeigten es nicht, weil sie den Bildpuffer vor
 * der Fensterausgabe lesen. Jetzt wird alles in einen Speicher-DC gezeichnet
 * und mit einem einzigen BitBlt uebertragen. */
static HDC back_buffer(WinPane *p, HDC dc, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    if (p->back_dc && p->back_w == w && p->back_h == h) return p->back_dc;
    if (p->back_dc) {
        SelectObject(p->back_dc, p->back_old);
        DeleteObject(p->back_bmp);
        DeleteDC(p->back_dc);
        p->back_dc = NULL;
    }
    p->back_dc = CreateCompatibleDC(dc);
    if (!p->back_dc) return NULL;
    p->back_bmp = CreateCompatibleBitmap(dc, w, h);
    if (!p->back_bmp) {
        DeleteDC(p->back_dc);
        p->back_dc = NULL;
        return NULL;
    }
    p->back_old = (HBITMAP)SelectObject(p->back_dc, p->back_bmp);
    p->back_w = w;
    p->back_h = h;
    return p->back_dc;
}

static void back_buffer_free(WinPane *p) {
    if (p->back_dc) {
        SelectObject(p->back_dc, p->back_old);
        DeleteObject(p->back_bmp);
        DeleteDC(p->back_dc);
        p->back_dc = NULL;
    }
}

static int overlay_lines(const void *pane, const char *out[10]) {
    int i, n = 0;
    if (pane != (const void *)&g_pane[0]) return 0;
    if (win_debug()) {
        for (i = 0; i < g_status_lines && i < 10; i++) out[n++] = g_status[i];
    } else if (win_fps_overlay() && g_status_lines > 0 && g_status[0][0]) {
        out[n++] = g_status[0];
    }
    return n;
}

static void draw_into(WinPane *p, HDC dc, const RECT *rc) {
    FillRect(dc, rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
    if (g_dib) {
        /* Verkleinern: HALFTONE mittelt (sonst fehlen ganze Zeilen, Text
           zerfaellt); Vergroessern: naechster Nachbar (scharf, billig). */
        if (p->vp.scale < 1.0) {
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, NULL);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
        }
        /* Quelle ab Zeile src_y als eigenes top-down-Bitmap (Hoehe src_h):
           vermeidet die Y-Ursprungs-Mehrdeutigkeit von StretchDIBits bei
           top-down-DIBs. */
        StretchDIBits(dc, p->vp.x, p->vp.y, p->vp.width, p->vp.height,
                      p->src_x, 0, p->src_w, p->src_h,
                      g_dib + (size_t)p->src_y * g_w * 4, &p->bi,
                      DIB_RGB_COLORS, SRCCOPY);
    }
    {
        const char *lines[10];
        int n = overlay_lines(p, lines);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0, 0, 0));          /* Schatten */
        for (int i = 0; i < n; i++)
            TextOutA(dc, 9, 9 + i * 16, lines[i], (int)strlen(lines[i]));
        SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < n; i++)
            TextOutA(dc, 8, 8 + i * 16, lines[i], (int)strlen(lines[i]));
    }
}

static void draw(WinPane *p, HDC dc) {
    RECT rc;
    HDC bdc;
    GetClientRect(p->hwnd, &rc);
    if (g_present_fn != NULL) {
        WinPresentInfo info;
        const char *lines[10];
        int nlines = overlay_lines(p, lines);
        memset(&info, 0, sizeof(info));
        info.hwnd = p->hwnd;
        info.pane = (int)(p - g_pane);
        info.client_w = rc.right - rc.left;
        info.client_h = rc.bottom - rc.top;
        info.vp = p->vp;
        info.src_x = p->src_x;
        info.src_y = p->src_y;
        info.src_w = p->src_w;
        info.src_h = p->src_h;
        info.status = lines;
        info.nstatus = nlines;
        if (g_present_fn(&info)) return;
    }
    bdc = back_buffer(p, dc, rc.right - rc.left, rc.bottom - rc.top);
    if (!bdc) {           /* Fallback: direkt (z. B. minimiert) */
        draw_into(p, dc, &rc);
        return;
    }
    draw_into(p, bdc, &rc);
    BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, bdc, 0, 0, SRCCOPY);
}

/* Fensterpixel -> Kanvaspixel (fuer Touch), ueber den Ausschnitt des Panes. */
static void mouse_to_canvas(WinPane *p, LPARAM lp) {
    int mx = (int)(short)LOWORD(lp);
    int my = (int)(short)HIWORD(lp);
    if (p == NULL || p->vp.scale <= 0) return;
    g_mouse_x = p->src_x + (int)floor((mx - p->vp.x) / p->vp.scale);
    g_mouse_y = p->src_y + (int)floor((my - p->vp.y) / p->vp.scale);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    WinPane *p = pane_of(hwnd);
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_SIZE:
            update_viewport(p);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (wp == VK_F11) {
                if (p != NULL) toggle_fullscreen(p);
                return 0;
            }
            if (wp < 256) {
                AcquireSRWLockExclusive(&g_in_srw);
                g_key_down[wp] = 1;
                ReleaseSRWLockExclusive(&g_in_srw);
                g_keys[wp] = 1;
            }
            return 0;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            if (wp < 256) {
                AcquireSRWLockExclusive(&g_in_srw);
                g_key_down[wp] = 0;
                ReleaseSRWLockExclusive(&g_in_srw);
            }
            return 0;
        case WM_KILLFOCUS:
            /* Fokuswechsel zwischen den eigenen Fenstern behaelt die Tasten. */
            if (pane_of((HWND)wp) == NULL) {
                AcquireSRWLockExclusive(&g_in_srw);
                memset(g_key_down, 0, sizeof(g_key_down));
                g_mouse_down = 0;
                ReleaseSRWLockExclusive(&g_in_srw);
            }
            return 0;
        case WM_MOUSEMOVE:
            AcquireSRWLockExclusive(&g_in_srw);
            mouse_to_canvas(p, lp);
            ReleaseSRWLockExclusive(&g_in_srw);
            return 0;
        case WM_LBUTTONDOWN:
            AcquireSRWLockExclusive(&g_in_srw);
            mouse_to_canvas(p, lp);
            g_mouse_down = 1;
            ReleaseSRWLockExclusive(&g_in_srw);
            SetCapture(hwnd);
            return 0;
        case WM_LBUTTONUP:
            AcquireSRWLockExclusive(&g_in_srw);
            g_mouse_down = 0;
            ReleaseSRWLockExclusive(&g_in_srw);
            ReleaseCapture();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (p != NULL) draw(p, dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

/* Testlaeufe (--deterministic): Fenster zeigen, ohne es zu aktivieren, damit
 * ein Lauf im Hintergrund nicht den Fokus stiehlt. */
static int g_no_activate = 0;
void win_set_no_activate(int on) {
    g_no_activate = on != 0;
}

/* Fenster mit Clientgroesse cw x ch; x/y = CW_USEDEFAULT oder Bildschirmpos. */
static HWND create_pane_window(const char *title, int x, int y, int cw, int ch) {
    RECT rc;
    HWND h;
    rc.left = 0;
    rc.top = 0;
    rc.right = cw;
    rc.bottom = ch;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    h = CreateWindowA("LaytonPortWindow", title,
                      WS_OVERLAPPEDWINDOW | (g_no_activate ? 0 : WS_VISIBLE),
                      x, y, rc.right - rc.left, rc.bottom - rc.top,
                      NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (h != NULL && g_no_activate) ShowWindow(h, SW_SHOWNOACTIVATE);
    return h;
}

int win_open(int width, int height, const char *title) {
    WNDCLASSA wc;
    int split;
    g_w = width;
    g_h = height;
    load_config();
    xinput_init();
    split = (g_layout_cli >= 0) ? g_layout_cli : g_layout_ini;

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "LaytonPortWindow";
    if (!RegisterClassA(&wc)) {
        return 0;
    }

    g_dib = (uint8_t *)calloc((size_t)g_w * g_h * 4, 1);
    if (!g_dib) {
        return 0;
    }

    memset(g_pane, 0, sizeof(g_pane));
    if (!split) {
        /* Beide Screens untereinander: 400x480, Standardmassstab 2x. */
        g_npanes = 1;
        pane_set_src(&g_pane[0], 0, 0, g_w, g_h);
        g_pane[0].hwnd = create_pane_window(title, CW_USEDEFAULT, CW_USEDEFAULT,
                                            g_w * 2, g_h * 2);
        if (!g_pane[0].hwnd) return 0;
    } else {
        /* Oberer Screen (obere Kanvashaelfte) und darunter das Touch-
           Rechteck (Standard bis win_set_touch_rect: untere Haelfte). */
        RECT wr;
        int bx, by;
        g_npanes = 2;
        pane_set_src(&g_pane[0], 0, 0, g_w, g_h / 2);
        pane_set_src(&g_pane[1], 0, g_h / 2, g_w, g_h - g_h / 2);
        g_pane[0].hwnd = create_pane_window(title, CW_USEDEFAULT, CW_USEDEFAULT,
                                            g_pane[0].src_w * 2,
                                            g_pane[0].src_h * 2);
        if (!g_pane[0].hwnd) return 0;
        GetWindowRect(g_pane[0].hwnd, &wr);
        bx = wr.left;
        by = wr.bottom;
        g_pane[1].hwnd = create_pane_window(title, bx, by,
                                            g_pane[1].src_w * 2,
                                            g_pane[1].src_h * 2);
        if (!g_pane[1].hwnd) return 0;
    }
    update_viewport(&g_pane[0]);
    update_viewport(&g_pane[1]);
    return 1;
}

int win_pump(void) {
    MSG msg;
    int i;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    for (i = 0; i < g_npanes; i++) {
        InvalidateRect(g_pane[i].hwnd, NULL, FALSE);
        UpdateWindow(g_pane[i].hwnd);
    }
    return g_npanes > 0 && g_pane[0].hwnd != NULL;
}

void win_blit(const uint8_t *rgb, int w, int h) {
    if (!g_dib || !rgb) {
        return;
    }
    int cw = (w < g_w) ? w : g_w;
    int ch = (h < g_h) ? h : g_h;
    for (int y = 0; y < ch; y++) {
        const uint8_t *src = rgb + (size_t)y * w * 3;
        uint8_t *dst = g_dib + (size_t)y * g_w * 4;
        for (int x = 0; x < cw; x++) {
            dst[x * 4 + 0] = src[x * 3 + 2];
            dst[x * 4 + 1] = src[x * 3 + 1];
            dst[x * 4 + 2] = src[x * 3 + 0];
            dst[x * 4 + 3] = 0;
        }
    }
}

void win_status(const char *text, int line) {
    if (line < 0 || line >= 10) {
        return;
    }
    snprintf(g_status[line], sizeof(g_status[line]), "%s", text ? text : "");
    if (line + 1 > g_status_lines) {
        g_status_lines = line + 1;
    }
}

void win_set_title(const char *text) {
    if (g_npanes > 0 && g_pane[0].hwnd) {
        SetWindowTextA(g_pane[0].hwnd, text ? text : "");
    }
    if (g_npanes > 1 && g_pane[1].hwnd) {
        char t[256];
        snprintf(t, sizeof(t), "%s - Touchscreen", text ? text : "");
        SetWindowTextA(g_pane[1].hwnd, t);
    }
}

void win_set_present(WinPresentFn present, WinDetachFn detach) {
    g_present_fn = present;
    g_detach_fn = detach;
}

int win_config_scale(void) {
    return g_scale_ini;
}

void win_set_debug(int on) {
    g_debug_cli = on != 0;
}

int win_debug(void) {
    return (g_debug_cli >= 0) ? g_debug_cli : g_debug_ini;
}

void win_close(void) {
    int i, n = g_npanes;
    g_npanes = 0;                 /* WndProc sieht ab hier keine Panes mehr */
    for (i = 0; i < n; i++) {
        back_buffer_free(&g_pane[i]);
        if (g_pane[i].hwnd && g_detach_fn) g_detach_fn(g_pane[i].hwnd);
        if (g_pane[i].hwnd) {
            DestroyWindow(g_pane[i].hwnd);
            g_pane[i].hwnd = NULL;
        }
    }
    free(g_dib);
    g_dib = NULL;
}

void win_set_touch_rect(int x, int y, int w, int h) {
    g_touch_x = x;
    g_touch_y = y;
    g_touch_w = w;
    g_touch_h = h;
    g_has_touch_rect = (w > 0 && h > 0);
    /* Split-Layout: das zweite Fenster zeigt genau den Touchscreen. */
    if (g_npanes > 1 && g_has_touch_rect) {
        WinPane *b = &g_pane[1];
        pane_set_src(b, x, y, w, h);
        if (!b->fullscreen && g_pane[0].hwnd && b->hwnd) {
            /* Clientgroesse 2x, zentriert unter dem oberen Fenster. */
            RECT top, rc = {0, 0, w * 2, h * 2};
            AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
            GetWindowRect(g_pane[0].hwnd, &top);
            SetWindowPos(b->hwnd, NULL,
                         top.left + ((top.right - top.left) -
                                     (rc.right - rc.left)) / 2,
                         top.bottom, rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        update_viewport(b);
    }
}

void win_input_poll(WinInput *out) {
    int mx, my, md;
    int down[256];
    HWND fg;
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    AcquireSRWLockShared(&g_in_srw);
    memcpy(down, g_key_down, sizeof(down));
    mx = g_mouse_x;          /* bereits Kanvaspixel (mouse_to_canvas) */
    my = g_mouse_y;
    md = g_mouse_down;
    ReleaseSRWLockShared(&g_in_srw);

    /* 3DS-Tastenbelegung aus port.ini (Standard siehe window.h/docs). */
    out->a = g_map.a && down[g_map.a] != 0;
    out->b = g_map.b && down[g_map.b] != 0;
    out->x = g_map.x && down[g_map.x] != 0;
    out->y = g_map.y && down[g_map.y] != 0;
    out->l = g_map.l && down[g_map.l] != 0;
    out->r = g_map.r && down[g_map.r] != 0;
    out->start = g_map.start && down[g_map.start] != 0;
    out->select = g_map.select && down[g_map.select] != 0;
    out->up = g_map.up && down[g_map.up] != 0;
    out->down = g_map.down && down[g_map.down] != 0;
    out->left = g_map.left && down[g_map.left] != 0;
    out->right = g_map.right && down[g_map.right] != 0;

    /* Circle-Pad wie Azahar analog_from_button: jede Achse -1/0/+1, beide
     * Achsen zugleich mit sqrt(1/2), dann * MAX_CIRCLEPAD_POS (0x9A) und
     * gerundet (hid.cpp). Damit liegt auch die Diagonale auf dem Einheits-
     * kreis und nicht bei (0x9A,0x9A). */
    {
        const double SQRT_HALF = 0.7071067811865476;
        int cxdir = 0, cydir = 0;
        if (g_map.circle_up && down[g_map.circle_up]) cydir += 1;
        if (g_map.circle_down && down[g_map.circle_down]) cydir -= 1;
        if (g_map.circle_left && down[g_map.circle_left]) cxdir -= 1;
        if (g_map.circle_right && down[g_map.circle_right]) cxdir += 1;
        out->circle_x = (int16_t)lround(cxdir * (cydir == 0 ? 1.0 : SQRT_HALF) * 0x9A);
        out->circle_y = (int16_t)lround(cydir * (cxdir == 0 ? 1.0 : SQRT_HALF) * 0x9A);
    }

    /* Optionales XInput-Gamepad zusaetzlich zur Tastatur. XInput ist global
     * (unabhaengig vom Fokus); wie die Tastatur (Fenstermeldungen) wirkt das
     * Gamepad nur, solange dieses Fenster im Vordergrund ist. Sonst steuerte
     * ein Controller alle laufenden Instanzen gleichzeitig. */
    fg = GetForegroundWindow();
    if (fg != NULL && pane_of(fg) != NULL)
        (void)xinput_poll(out);

    if (g_has_touch_rect) {
        int px = mx, py = my, tx, ty;
        tx = px - g_touch_x;
        ty = py - g_touch_y;
        if (tx < 0) tx = 0;
        if (tx >= g_touch_w) tx = g_touch_w - 1;
        if (ty < 0) ty = 0;
        if (ty >= g_touch_h) ty = g_touch_h - 1;
        out->touch_x = tx;
        out->touch_y = ty;
        out->touch_pressed = md != 0 &&
            px >= g_touch_x && px < g_touch_x + g_touch_w &&
            py >= g_touch_y && py < g_touch_y + g_touch_h;
    }
}

int win_key_take(int vk) {
    int v;
    if (vk < 0 || vk > 255) return 0;
    v = g_keys[vk];
    g_keys[vk] = 0;
    return v;
}

void win_key_push(int vk) {
    if (vk >= 0 && vk <= 255) {
        g_keys[vk] = 1;
        AcquireSRWLockExclusive(&g_in_srw);
        g_key_down[vk] = 1;
        ReleaseSRWLockExclusive(&g_in_srw);
    }
}
