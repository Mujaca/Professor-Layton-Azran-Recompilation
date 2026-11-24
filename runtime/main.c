/* Port-Host: laedt den Spielcode, startet den recompilierten Einstieg
 * 0x00100000 im Gast-Thread und zeigt die vom Spiel gemeldeten
 * Framebuffer (gsp::Gpu SetBufferSwap) an.
 *
 *   port.exe [romfs-dir] [--shot datei.bmp] [--frames n]
 *
 * Hier gibt es bewusst keine Spieladressen und keine Ersatzbilder: Was im
 * Fenster erscheint, hat das Spiel selbst in seinen Framebuffer geschrieben.
 */
#include "arm_cpu.h"
#include "arm_trace.h"
#include "gpu.h"
#include "hle.h"
#include "window.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MEM_SIZE 0x30000000u
#define CODE_BASE 0x00100000u
#define CODE_MAX 0x00300000u

/* Fenster: oberer Screen 400x240, darunter mittig der untere 320x240. */
#define FB_W 400
#define FB_H 480
#define BOT_X 40
#define BOT_Y 240

void fn_00100000(ArmCpu *c);

static unsigned char *g_mem = NULL;
static ArmCpu g_cpu;
static volatile int g_halt = 0;
static unsigned char g_rgb[FB_W * FB_H * 3];

static DWORD WINAPI game_thread(LPVOID unused) {
    (void)unused;
    /* Der Gast-Lock gehoert dem Thread, der Gastcode ausfuehrt. hle_init()
       laeuft dagegen auf dem Host-Hauptthread und darf ihn nicht nehmen. */
    hle_guest_enter();
    fn_00100000(&g_cpu);
    hle_guest_leave();
    g_halt = 1;
    return 0;
}

/* 3DS-Framebuffer sind hochkant gespeichert (240 Pixel je Zeile, Zeilen
 * entlang der Bildschirmbreite): Pixel (x, y) liegt bei
 * addr + x * stride + (239 - y) * bpp. */
static void present_screen(int screen, int dx, int dy, int w) {
    uint32_t addr = hle_gsp_fb_addr(screen) & 0x1FFFFFFFu;
    uint32_t stride = 0, fmt = 1;
    int bpp;
    hle_gsp_fb_layout(screen, &stride, &fmt);
    fmt &= 7u;
    bpp = (fmt == 0) ? 4 : (fmt == 1) ? 3 : 2;
    if (stride == 0) stride = 240u * (uint32_t)bpp;
    if ((uint64_t)addr + (uint64_t)stride * (uint64_t)w > MEM_SIZE) return;
    for (int x = 0; x < w; x++) {
        const unsigned char *col = g_mem + addr + (size_t)x * stride;
        for (int y = 0; y < 240; y++) {
            const unsigned char *p = col + (size_t)(239 - y) * (size_t)bpp;
            unsigned char *o = g_rgb + ((size_t)(dy + y) * FB_W + (size_t)(dx + x)) * 3;
            unsigned r, g, b;
            uint16_t v;
            switch (fmt) {
            case 0: r = p[3]; g = p[2]; b = p[1]; break;          /* RGBA8 */
            case 1: r = p[2]; g = p[1]; b = p[0]; break;          /* BGR8 */
            case 2: v = (uint16_t)(p[0] | (p[1] << 8));            /* RGB565 */
                    r = (v >> 11) << 3; g = ((v >> 5) & 63u) << 2; b = (v & 31u) << 3; break;
            case 3: v = (uint16_t)(p[0] | (p[1] << 8));            /* RGB5A1 */
                    r = (v >> 11) << 3; g = ((v >> 6) & 31u) << 3; b = ((v >> 1) & 31u) << 3; break;
            default: v = (uint16_t)(p[0] | (p[1] << 8));           /* RGBA4 */
                    r = (v >> 12) << 4; g = ((v >> 8) & 15u) << 4; b = ((v >> 4) & 15u) << 4; break;
            }
            o[0] = (unsigned char)r; o[1] = (unsigned char)g; o[2] = (unsigned char)b;
        }
    }
}

static void compose_frame(void) {
    memset(g_rgb, 0, sizeof(g_rgb));
    present_screen(0, 0, 0, 400);
    present_screen(1, BOT_X, BOT_Y, 320);
}

static void save_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    int row = FB_W * 3, data_size = row * FB_H, total = 54 + data_size;
    int off = 54, ih = 40, w = FB_W, h = FB_H;
    short planes = 1, bpp = 24;
    unsigned char hdr[54] = {0};
    if (!f) return;
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &ih, 4);
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &data_size, 4);
    fwrite(hdr, 1, 54, f);
    for (int y = FB_H - 1; y >= 0; y--) {
        const unsigned char *src = g_rgb + (size_t)y * FB_W * 3;
        for (int x = 0; x < FB_W; x++) {
            unsigned char bgr[3] = {src[x * 3 + 2], src[x * 3 + 1], src[x * 3]};
            fwrite(bgr, 1, 3, f);
        }
    }
    fclose(f);
    printf("Screenshot: %s\n", path);
}

int main(int argc, char **argv) {
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    int shot_frames = 600;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) shot_frames = atoi(argv[++i]);
        else romfs = argv[i];
    }

    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) { printf("kein Speicher\n"); return 2; }
    {
        FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
        size_t n;
        if (!f) { printf("out/exefs/code_decomp.bin fehlt (python tools/build.py)\n"); return 2; }
        n = fread(g_mem + CODE_BASE, 1, CODE_MAX, f);
        fclose(f);
        printf("code: %zu Bytes geladen\n", n);
    }

    arm_cpu_init(&g_cpu, g_mem, MEM_SIZE);
    g_cpu.r[13] = 0x0FFF0000u;
    g_cpu.tpidruro = 0x1FF82000u;
    g_cpu.r[15] = CODE_BASE;
    hle_init(&g_cpu, romfs);
    arm_set_svc_handler(hle_svc);

    if (!win_open(FB_W, FB_H, "Professor Layton and the Azran Legacy")) {
        printf("Fenster konnte nicht geoeffnet werden\n");
        return 3;
    }
    if (!CreateThread(NULL, 0, game_thread, NULL, 0, NULL)) {
        printf("Thread fehlgeschlagen\n");
        return 3;
    }

    int frames = 0;
    while (win_pump()) {
        char line[128];
        compose_frame();
        win_blit(g_rgb, FB_W, FB_H);
        if ((frames % 30) == 0) {
            snprintf(line, sizeof(line), "frames=%d  block=0x%08X  halted=%d  lists=%llu draws=%llu",
                     frames, arm_last_block, g_halt, gpu_pica_list_count(), gpu_pica_draw_count());
            win_status(line, 1);
            if (frames % 300 == 0) {
                printf("[port] %s\n", line);
                /* Diagnose (HLE_STALLDUMP): letzte Bloecke je Gast-Thread,
                   um einen Haenger ohne IPC zu lokalisieren. */
                if (getenv("HLE_STALLDUMP")) {
                    int ti;
                    for (ti = 0; ti < 16; ti++)
                        if (arm_thread_last_pc[ti] != 0u)
                            arm_trace_thread_dump(ti, 12);
                }
            }
        }
        frames++;
        Sleep(16);
        if (shot != NULL && frames >= shot_frames) {
            save_bmp(shot);
            break;
        }
    }
    win_close();
    hle_report();
    return 0;
}
