/* Port-Host: fuehrt den recompilierten Code aus und zeigt den Framebuffer. */
#include "arm_cpu.h"
#include "arm_trace.h"
#include "hle.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MEM_SIZE 0x30000000u
#define FB_ADDR  0x1F000000u
#define FB_W 400
#define FB_H 240

void fn_00100000(ArmCpu *c);

static unsigned char *g_mem = NULL;
static ArmCpu g_cpu;
static volatile int g_halt = 0;
static unsigned char g_rgb[FB_W * FB_H * 3];

static DWORD WINAPI game_thread(LPVOID unused) {
    (void)unused;
    fn_00100000(&g_cpu);
    g_halt = 1;
    return 0;
}

/* SMDH-Icon (48x48 RGB565 bei 0x24C0) ins Framebuffer zeichnen. */
static void draw_icon(void) {
    FILE *f = fopen("out/exefs/icon.bin", "rb");
    if (!f) return;
    unsigned char smdh[0x36C0];
    size_t n = fread(smdh, 1, sizeof(smdh), f);
    fclose(f);
    if (n < 0x36C0) return;

    const unsigned char *icon = smdh + 0x24C0;
    int scale = 4;
    int ox = (FB_W - 48 * scale) / 2;
    int oy = (FB_H - 48 * scale) / 2;

    for (int y = 0; y < 48 * scale; y++) {
        int sy = y / scale;
        for (int x = 0; x < 48 * scale; x++) {
            int sx = x / scale;
            unsigned v = (unsigned)icon[(sy * 48 + sx) * 2]
                       | ((unsigned)icon[(sy * 48 + sx) * 2 + 1] << 8);
            int r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
            int py = oy + y, px = ox + x;
            if (py < 0 || py >= FB_H || px < 0 || px >= FB_W) continue;
            unsigned char *dst = g_rgb + ((size_t)py * FB_W + px) * 3;
            dst[0] = (unsigned char)((r << 3) | (r >> 2));
            dst[1] = (unsigned char)((g << 2) | (g >> 4));
            dst[2] = (unsigned char)((b << 3) | (b >> 2));
        }
    }
    /* Titel (UTF-16LE, erste Sprache) fuer die Statuszeile holen */
    char title[80] = {0};
    for (int i = 0; i < 78 && smdh[0x08 + i * 2 + 1] == 0; i++) {
        title[i] = (char)smdh[0x08 + i * 2];
        if (title[i] == 0) break;
    }
    win_status(title[0] ? title : "Professor Layton and the Azran Legacy", 2);
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int row = FB_W * 3;
    int pad = (4 - (row % 4)) % 4;
    int data_size = (row + pad) * FB_H;
    unsigned char hdr[54] = {0};
    hdr[0] = 'B'; hdr[1] = 'M';
    int total = 54 + data_size;
    memcpy(hdr + 2, &total, 4);
    int off = 54;
    memcpy(hdr + 10, &off, 4);
    int ih = 40;
    memcpy(hdr + 14, &ih, 4);
    int w = FB_W, h = FB_H;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    short planes = 1, bpp = 24;
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, f);
    unsigned char zeros[3] = {0, 0, 0};
    for (int y = FB_H - 1; y >= 0; y--) {
        const unsigned char *src = g_rgb + (size_t)y * FB_W * 3;
        for (int x = 0; x < FB_W; x++) {
            unsigned char bgr[3] = {src[x * 3 + 2], src[x * 3 + 1], src[x * 3 + 0]};
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(zeros, 1, pad, f);
    }
    fclose(f);
    printf("Screenshot: %s\n", path);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else romfs = argv[i];
    }

    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) { printf("kein Speicher\n"); return 2; }

    FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
    if (!f) { printf("code_decomp.bin fehlt\n"); return 2; }
    size_t n = fread(g_mem + 0x100000, 1, 0x300000, f);
    fclose(f);
    printf("code: %zu Bytes geladen\n", n);

    arm_cpu_init(&g_cpu, g_mem, MEM_SIZE);
    g_cpu.r[13] = 0x0FFF0000u;
    g_cpu.tpidruro = 0x1FF82000u;
    g_cpu.r[15] = 0x00100000u;

    hle_init(&g_cpu, romfs);
    arm_set_svc_handler(hle_svc);

    if (!win_open(FB_W, FB_H, "Professor Layton and the Azran Legacy - Port")) {
        printf("Fenster konnte nicht geoeffnet werden\n");
        return 3;
    }
    win_status("Layton 6 Port - recompiliert (ARM11 -> C)", 0);
    win_status("boot...", 1);
    draw_icon();
    memcpy(g_rgb, g_mem + FB_ADDR, sizeof(g_rgb));

    HANDLE th = CreateThread(NULL, 0, game_thread, NULL, 0, NULL);
    if (!th) { printf("Thread fehlgeschlagen\n"); return 3; }

    int frames = 0;
    while (win_pump()) {
        memcpy(g_rgb, g_mem + FB_ADDR, sizeof(g_rgb));
        draw_icon();
        win_blit(g_rgb, FB_W, FB_H);

        static int tick = 0;
        if ((++tick % 30) == 0) {
            char line[192];
            snprintf(line, sizeof(line), "PC=0x%08X  halted=%d  frames=%d",
                     g_cpu.r[15], g_halt, frames);
            win_status(line, 1);
        }
        frames++;
        Sleep(16);
        if (shot != NULL && frames == 120) {
            save_bmp(shot);
        }
        if (shot != NULL && frames > 130) {
            break;
        }
        if (g_halt && shot == NULL) {
            /* Fenster bleibt offen, bis es geschlossen wird */
        }
    }
    if (shot) save_bmp(shot);
    win_close();
    hle_report();
    return 0;
}
