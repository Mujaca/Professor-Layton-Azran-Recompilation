/* Port-Host: laedt den Spielcode, startet den recompilierten Einstieg
 * 0x00100000 im Gast-Thread und zeigt beide Bildschirme (oben 400x240, unten
 * 320x240) aus den Framebuffern, die das Spiel per GSP-FrameBufferInfo /
 * SetBufferSwap gemeldet hat. Der sichtbare Puffer wird beim VBlank
 * uebernommen (runtime/kernel/hle.c), dekodiert in
 * runtime/platform/framebuffer.h.
 *
 *   port.exe [romfs-dir] [--shot datei.bmp] [--frames n]
 *
 * --shot speichert beide Screens untereinander als BMP; --frames n beendet
 * den Lauf nach n Host-Frames (ohne Angabe laeuft das Fenster bis zum
 * Schliessen).
 *
 * Hier gibt es bewusst keine Spieladressen und keine Ersatzbilder: Was im
 * Fenster erscheint, hat das Spiel selbst in seinen Framebuffer geschrieben.
 */
#include "arm_cpu.h"
#include "arm_trace.h"
#include "frame_clock.h"
#include "framebuffer.h"
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

/* Fenster: oberer Screen 400x240, darunter mittig der untere 320x240
 * (Groessen und Layout stehen in runtime/platform/framebuffer.h). */
#define FB_W FB_CANVAS_W
#define FB_H FB_CANVAS_H
#define BOT_X FB_BOTTOM_X
#define BOT_Y FB_BOTTOM_Y

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

/* Baut das Bild aus den Framebindungen, die die GSP beim letzten VBlank
 * uebernommen hat. Es wird nichts nachgebaut: gelesen werden ausschliesslich
 * die Puffer, die das Spiel per SetBufferSwap/Framebuffer-Info gemeldet hat.
 * 3DS-Framebuffer sind spaltenweise: Pixel (x,y) bei
 * address + x*stride + (239-y)*bpp (siehe framebuffer.h). */
static void compose_frame(void) {
    HleGspFrame top, bottom;
    FbScreen ft, fb;
    hle_gsp_frame(0, &top);
    hle_gsp_frame(1, &bottom);
    ft.valid = top.valid;
    ft.address = top.address;
    ft.stride = top.stride;
    ft.format = top.format;
    fb.valid = bottom.valid;
    fb.address = bottom.address;
    fb.stride = bottom.stride;
    fb.format = bottom.format;
    fb_compose(g_rgb, FB_W, FB_H, g_mem, MEM_SIZE, &ft, &fb);
}

static void save_shot(const char *path) {
    if (fb_write_bmp(path, g_rgb, FB_W, FB_H)) {
        printf("Screenshot: %s (%dx%d: oben %dx%d, unten %dx%d)\n", path, FB_W,
               FB_H, FB_TOP_COLUMNS, FB_TOP_ROWS, FB_BOTTOM_COLUMNS, FB_BOTTOM_ROWS);
    } else {
        printf("Screenshot fehlgeschlagen: %s\n", path);
    }
}

int main(int argc, char **argv) {
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    int frames_limit = -1;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames_limit = atoi(argv[++i]);
        else romfs = argv[i];
    }
    if (shot != NULL && frames_limit < 0) frames_limit = 600;

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
    /* Unterer Screen als Touchflaeche; Tastenbelegung siehe window.h. */
    win_set_touch_rect(BOT_X, BOT_Y, 320, 240);
    win_status("Tasten: port.ini neben port.exe (Standard siehe docs/FRONTEND.md)", 2);
    win_status("F11=Vollbild  Touch=Maus ueber dem unteren Screen", 3);
    if (!CreateThread(NULL, 0, game_thread, NULL, 0, NULL)) {
        printf("Thread fehlgeschlagen\n");
        return 3;
    }

    int frames = 0;
    unsigned guest_frames = 0;
    FrameClock clock;
    uint64_t fps_t0;
    int fps_frames0 = 0;
    double host_fps = 0.0;
    frame_clock_start(&clock);
    fps_t0 = frame_clock_now_qpc();
    while (win_pump()) {
        char line[160];
        WinInput wi;
        HleHidInput hi;
        if (frames_limit >= 0 && frames >= frames_limit) {
            if (shot != NULL) save_shot(shot);
            break;
        }
        /* Host-Eingabe je Frame; der Runtime wendet sie in hid_poll unter dem
           Gast-Lock an (nur Host-Puffer, nie direkter Gastzugriff). */
        win_input_poll(&wi);
        memset(&hi, 0, sizeof(hi));
        hi.a = (uint32_t)wi.a; hi.b = (uint32_t)wi.b;
        hi.x = (uint32_t)wi.x; hi.y = (uint32_t)wi.y;
        hi.l = (uint32_t)wi.l; hi.r = (uint32_t)wi.r;
        hi.start = (uint32_t)wi.start; hi.select = (uint32_t)wi.select;
        hi.up = (uint32_t)wi.up; hi.down = (uint32_t)wi.down;
        hi.left = (uint32_t)wi.left; hi.right = (uint32_t)wi.right;
        hi.circle_x = wi.circle_x; hi.circle_y = wi.circle_y;
        hi.touch_pressed = wi.touch_pressed;
        hi.touch_x = (uint16_t)wi.touch_x; hi.touch_y = (uint16_t)wi.touch_y;
        hle_hid_set_input(&hi);

        compose_frame();
        win_blit(g_rgb, FB_W, FB_H);
        if ((frames % 30) == 0) {
            HleGspFrame top, bottom;
            char title[160];
            uint64_t now = frame_clock_now_qpc();
            uint64_t elapsed = now - fps_t0;
            hle_gsp_frame(0, &top);
            hle_gsp_frame(1, &bottom);
            if (elapsed > 0) {
                host_fps = (double)(frames - fps_frames0) * (double)clock.freq /
                           (double)elapsed;
            }
            fps_t0 = now;
            fps_frames0 = frames;
            guest_frames = hle_gsp_guest_frames();
            snprintf(title, sizeof(title),
                     "Professor Layton and the Azran Legacy  |  fps=%.1f  |  "
                     "Gast-Frames=%u  |  F11=Vollbild",
                     host_fps, guest_frames);
            win_set_title(title);
            snprintf(line, sizeof(line),
                     "frames=%d  fps=%.1f gast=%u  block=0x%08X  halted=%d  "
                     "fb0=%d:0x%08X/%u/%X fb1=%d:0x%08X/%u/%X",
                     frames, host_fps, guest_frames, arm_last_block, g_halt,
                     top.valid, top.address, top.stride, top.format & 7u,
                     bottom.valid, bottom.address, bottom.stride,
                     bottom.format & 7u);
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
        frame_clock_wait(&clock);
    }
    frame_clock_stop(&clock);
    if (shot != NULL && frames_limit < 0) save_shot(shot);
    win_close();
    hle_report();
    return 0;
}
