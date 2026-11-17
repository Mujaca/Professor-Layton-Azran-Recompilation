#include "window.h"

#include <windows.h>
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
            if (wp < 256) g_keys[wp] = 1;
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

int win_key_take(int vk) {
    int v;
    if (vk < 0 || vk > 255) return 0;
    v = g_keys[vk];
    g_keys[vk] = 0;
    return v;
}

void win_key_push(int vk) {
    if (vk >= 0 && vk <= 255) g_keys[vk] = 1;
}
