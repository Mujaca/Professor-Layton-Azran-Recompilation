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

/* --shot-every N: zusaetzlich alle N Host-Frames ein Bild
 * ("name_000600.bmp"), mit --shot-from M erst ab Host-Frame M (z. B. fuer
 * Flacker-Analysen mit --shot-every 1). Reine Diagnose, kein Gastzustand. */
static void save_shot_numbered(const char *path, int frame) {
    char out[512];
    size_t n = strlen(path);
    size_t dot = n;
    while (dot > 0 && path[dot - 1] != '.') dot--;
    if (dot == 0) {                              /* kein Punkt: anhaengen */
        snprintf(out, sizeof(out), "%s_%06d", path, frame);
    } else {
        snprintf(out, sizeof(out), "%.*s_%06d%s", (int)(dot - 1), path, frame, path + dot - 1);
    }
    save_shot(out);
}

/* Skriptbare Host-Eingabe fuer Testlaeufe: --press start@300:10,a@600
 * (Taste@Host-Frame[:Dauer], Dauer Standard 10 Frames). `touch` nimmt
 * zusaetzlich Koordinaten auf dem unteren Screen:
 * touch@9000:20:160:120 (Standard 160,120). Geht denselben Weg wie
 * Tastatur/Gamepad (hle_hid_set_input), veraendert keinen Gastzustand. */
typedef struct { char key[8]; int at; int dur; int tx, ty; } PressEv;
static PressEv g_press[32];
static int g_press_n = 0;

static void press_parse(const char *spec) {
    char buf[512];
    char *tok, *save = NULL;
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok_s(buf, ",", &save); tok && g_press_n < 32; tok = strtok_s(NULL, ",", &save)) {
        PressEv *e = &g_press[g_press_n];
        char *at = strchr(tok, '@');
        if (!at) continue;
        *at = 0;
        snprintf(e->key, sizeof(e->key), "%s", tok);
        e->dur = 10;
        e->tx = 160;
        e->ty = 120;
        if (sscanf(at + 1, "%d:%d:%d:%d", &e->at, &e->dur, &e->tx, &e->ty) >= 1) g_press_n++;
    }
}

static void press_apply(HleHidInput *hi, int frame) {
    for (int i = 0; i < g_press_n; i++) {
        PressEv *e = &g_press[i];
        if (frame < e->at || frame >= e->at + e->dur) continue;
        if (!strcmp(e->key, "a")) hi->a = 1;
        else if (!strcmp(e->key, "b")) hi->b = 1;
        else if (!strcmp(e->key, "x")) hi->x = 1;
        else if (!strcmp(e->key, "y")) hi->y = 1;
        else if (!strcmp(e->key, "start")) hi->start = 1;
        else if (!strcmp(e->key, "select")) hi->select = 1;
        else if (!strcmp(e->key, "touch")) {
            hi->touch_pressed = 1;
            hi->touch_x = (uint16_t)(e->tx < 0 ? 0 : e->tx > 319 ? 319 : e->tx);
            hi->touch_y = (uint16_t)(e->ty < 0 ? 0 : e->ty > 239 ? 239 : e->ty);
        }
    }
}

int main(int argc, char **argv) {
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    int frames_limit = -1;
    int shot_every = 0;
    int shot_from = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--shot-every") == 0 && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot-from") == 0 && i + 1 < argc) shot_from = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames_limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc) press_parse(argv[++i]);
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
        press_apply(&hi, frames);
        hle_hid_set_input(&hi);

        compose_frame();
        win_blit(g_rgb, FB_W, FB_H);
        if (shot != NULL && shot_every > 0 && frames > 0 && frames >= shot_from &&
            (frames % shot_every) == 0)
            save_shot_numbered(shot, frames);
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
                        if (arm_thread_last_pc[ti] != 0u) {
                            arm_trace_thread_dump(ti, 12);
                            arm_trace_thread_regs(ti);
                        }
                }
            }
        }
        frames++;
        frame_clock_wait(&clock);
    }
    frame_clock_stop(&clock);
    if (shot != NULL && frames_limit < 0) save_shot(shot);
    {
        /* Diagnose (HLE_VRAM_DUMP=<pfad>): VRAM roh ausgeben, um
           Farbziel-/Transferziele unabhaengig pruefen zu koennen. */
        const char *vd = getenv("HLE_VRAM_DUMP");
        if (vd != NULL && *vd != 0) {
            FILE *f = fopen(vd, "wb");
            if (f != NULL) {
                fwrite(g_mem + 0x1F000000u, 1, 0x600000u, f);
                fclose(f);
                printf("VRAM-Dump: %s (0x600000 Byte)\n", vd);
            }
        }
    }
    win_close();
    hle_report();
    return 0;
}
