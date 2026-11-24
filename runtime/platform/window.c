#include "window.h"

#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static HWND g_hwnd = NULL;
static BITMAPINFO g_bi;
static uint8_t *g_dib = NULL;
static int g_w = 400, g_h = 240;
static char g_status[10][192];
static int g_status_lines = 0;
static int g_scale = 2;
static int g_keys[256];
/* Tastatur-/Mauszustand fuer HID. Der WndProc-Hostthread schreibt, der
 * Port-Hauptthread liest; SRWLOCK schuetzt die Momentaufnahme. */
static SRWLOCK g_in_srw = SRWLOCK_INIT;
static int g_key_down[256];
static int g_mouse_x = -1, g_mouse_y = -1;
static int g_mouse_down = 0;
static int g_touch_x = 0, g_touch_y = 240, g_touch_w = 320, g_touch_h = 240;
static int g_has_touch_rect = 0;

static void draw(HDC dc) {
    if (g_dib) {
        StretchDIBits(dc, 0, 0, g_w * g_scale, g_h * g_scale,
                      0, 0, g_w, g_h, g_dib, &g_bi, DIB_RGB_COLORS, SRCCOPY);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < g_status_lines; i++) {
        TextOutA(dc, 8, 8 + i * 16, g_status[i], (int)strlen(g_status[i]));
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
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
            AcquireSRWLockExclusive(&g_in_srw);
            memset(g_key_down, 0, sizeof(g_key_down));
            g_mouse_down = 0;
            ReleaseSRWLockExclusive(&g_in_srw);
            return 0;
        case WM_MOUSEMOVE:
            AcquireSRWLockExclusive(&g_in_srw);
            g_mouse_x = (int)(short)LOWORD(lp);
            g_mouse_y = (int)(short)HIWORD(lp);
            ReleaseSRWLockExclusive(&g_in_srw);
            return 0;
        case WM_LBUTTONDOWN:
            AcquireSRWLockExclusive(&g_in_srw);
            g_mouse_x = (int)(short)LOWORD(lp);
            g_mouse_y = (int)(short)HIWORD(lp);
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
            draw(dc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        default:
            return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

int win_open(int width, int height, const char *title) {
    g_w = width;
    g_h = height;
    /* 400x480 (beide Screens) im Massstab 1:1; einzelne Screens 2x. */
    g_scale = (height > 240) ? 1 : 2;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = "LaytonPortWindow";
    if (!RegisterClassA(&wc)) {
        return 0;
    }

    RECT rc = {0, 0, g_w * g_scale, g_h * g_scale};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("LaytonPortWindow", title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd) {
        return 0;
    }

    memset(&g_bi, 0, sizeof(g_bi));
    g_bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    g_bi.bmiHeader.biWidth = g_w;
    g_bi.bmiHeader.biHeight = -g_h;   /* top-down */
    g_bi.bmiHeader.biPlanes = 1;
    g_bi.bmiHeader.biBitCount = 32;
    g_bi.bmiHeader.biCompression = BI_RGB;
    g_dib = (uint8_t *)calloc((size_t)g_w * g_h * 4, 1);
    if (!g_dib) {
        return 0;
    }
    return 1;
}

int win_pump(void) {
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            return 0;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
    UpdateWindow(g_hwnd);
    return g_hwnd != NULL;
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

void win_close(void) {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = NULL;
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
}

void win_input_poll(WinInput *out) {
    int mx, my, md;
    int down[256];
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    AcquireSRWLockShared(&g_in_srw);
    memcpy(down, g_key_down, sizeof(down));
    mx = g_mouse_x;
    my = g_mouse_y;
    md = g_mouse_down;
    ReleaseSRWLockShared(&g_in_srw);

    /* 3DS-Tastenbelegung (in window.h dokumentiert). */
    out->a = down['X'] != 0;
    out->b = down['Z'] != 0;
    out->x = down['S'] != 0;
    out->y = down['A'] != 0;
    out->l = down['Q'] != 0;
    out->r = down['W'] != 0;
    out->start = down[VK_RETURN] != 0;
    out->select = down[VK_BACK] != 0;
    out->up = down[VK_UP] != 0;
    out->down = down[VK_DOWN] != 0;
    out->left = down[VK_LEFT] != 0;
    out->right = down[VK_RIGHT] != 0;

    /* Circle-Pad wie Azahar analog_from_button: jede Achse -1/0/+1, beide
     * Achsen zugleich mit sqrt(1/2), dann * MAX_CIRCLEPAD_POS (0x9A) und
     * gerundet (hid.cpp). Damit liegt auch die Diagonale auf dem Einheits-
     * kreis und nicht bei (0x9A,0x9A). */
    {
        const double SQRT_HALF = 0.7071067811865476;
        int cxdir = 0, cydir = 0;
        if (down['I']) cydir += 1;
        if (down['K']) cydir -= 1;
        if (down['J']) cxdir -= 1;
        if (down['L']) cxdir += 1;
        out->circle_x = (int16_t)lround(cxdir * (cydir == 0 ? 1.0 : SQRT_HALF) * 0x9A);
        out->circle_y = (int16_t)lround(cydir * (cxdir == 0 ? 1.0 : SQRT_HALF) * 0x9A);
    }

    if (g_has_touch_rect && g_scale > 0) {
        int px = mx / g_scale, py = my / g_scale;
        int tx = px - g_touch_x, ty = py - g_touch_y;
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
