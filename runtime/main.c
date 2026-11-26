/* Port-Host: laedt den Spielcode, startet den recompilierten Einstieg
 * 0x00100000 im Gast-Thread und zeigt beide Bildschirme (oben 400x240, unten
 * 320x240) aus den Framebuffern, die das Spiel per GSP-FrameBufferInfo /
 * SetBufferSwap gemeldet hat. Der sichtbare Puffer wird beim VBlank
 * uebernommen (runtime/kernel/hle.c), dekodiert in
 * runtime/platform/framebuffer.h.
 *
 *   port.exe [romfs-dir] [--shot datei.bmp] [--frames n] [--savedata ordner]
 *
 * --shot speichert beide Screens untereinander als BMP; --frames n beendet
 * den Lauf nach n Host-Frames (ohne Angabe laeuft das Fenster bis zum
 * Schliessen). --savedata ordner ersetzt out/savedata als Spielstand-Wurzel
 * (tools/scene.py arbeitet so auf Fixture-Kopien).
 *
 * --deterministic: reproduzierbare Laeufe (M18). Alle gastsichtbaren Uhren
 * laufen auf virtueller Zeit (runtime/kernel/vclock.h), und jeder Durchlauf
 * der Fensterschleife gehoert zu genau einem VBlank: der Gast steht, waehrend
 * hier Bild, Screenshots und --press-Eingaben behandelt werden. Host-Frame =
 * VBlank-Nummer; zwei Laeufe mit denselben Argumenten liefern dieselbe
 * Bildfolge. Der Lauf ist nicht an die Wanduhr gebunden (Ton nur als WAV
 * sinnvoll). Echte Maus-/Tasten-/Gamepad-Eingaben werden ignoriert (nur
 * --press wirkt), das Fenster erscheint ohne Fokus. Ohne den Schalter bleibt
 * alles beim Echtzeitbetrieb.
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
#include "hw_d3d11.h"
#include "hw_present.h"
#include "logtee.h"
#include "lua_host.h"
#include "press_spec.h"
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
static HleGspFrame g_top, g_bottom;     /* Stand des letzten compose_frame */
static int g_shot_scaled = 0;           /* --shot-scaled */

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
    g_top = top;
    g_bottom = bottom;
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

/* M17: Kanvas in interner Aufloesung auf der GPU (HW-Renderer). Dieselben
 * Framebindungen wie compose_frame; Bildschirme ohne GPU-Anzeigeflaeche
 * kommen aus g_rgb. */
static void present_frame(void) {
    HwpScreen scr[2];
    memset(scr, 0, sizeof(scr));
    scr[0].valid = g_top.valid;
    scr[0].address = g_top.address;
    scr[0].stride = g_top.stride;
    scr[0].format = g_top.format;
    scr[0].dx = 0;
    scr[0].dy = 0;
    scr[0].columns = FB_TOP_COLUMNS;
    scr[0].rows = FB_TOP_ROWS;
    scr[1].valid = g_bottom.valid;
    scr[1].address = g_bottom.address;
    scr[1].stride = g_bottom.stride;
    scr[1].format = g_bottom.format;
    scr[1].dx = BOT_X;
    scr[1].dy = BOT_Y;
    scr[1].columns = FB_BOTTOM_COLUMNS;
    scr[1].rows = FB_BOTTOM_ROWS;
    hwp_frame(g_mem, MEM_SIZE, scr, g_rgb);
}

static int present_pane(const WinPresentInfo *in) {
    return hwp_present(in->hwnd, in->client_w, in->client_h, in->vp.x, in->vp.y,
                       in->vp.width, in->vp.height, in->src_x, in->src_y,
                       in->src_w, in->src_h, in->status, in->nstatus);
}

static void save_shot(const char *path) {
    if (fb_write_bmp(path, g_rgb, FB_W, FB_H)) {
        printf("Screenshot: %s (%dx%d: oben %dx%d, unten %dx%d)\n", path, FB_W,
               FB_H, FB_TOP_COLUMNS, FB_TOP_ROWS, FB_BOTTOM_COLUMNS, FB_BOTTOM_ROWS);
    } else {
        printf("Screenshot fehlgeschlagen: %s\n", path);
    }
    /* --shot-scaled: zusaetzlich die Anzeige in interner Aufloesung als
       "hd_<name>" daneben (nur HW-Renderer; das native Bild bleibt die
       Referenz fuer Vergleiche). */
    if (g_shot_scaled && hwp_active()) {
        char hd[600];
        const char *base = path, *q;
        for (q = path; *q; q++)
            if (*q == '/' || *q == '\\') base = q + 1;
        snprintf(hd, sizeof(hd), "%.*shd_%s", (int)(base - path), path, base);
        if (hwp_save_bmp(hd)) printf("Screenshot intern: %s\n", hd);
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
 * (Taste@Host-Frame[:Dauer], Dauer Standard 10 Frames), mit Praefix `g`
 * in Gast-Frames (touch@g1200:20:160:118). `touch` nimmt zusaetzlich
 * Koordinaten auf dem unteren Screen (Standard 160,120). Parser in
 * runtime/platform/press_spec.h. Geht denselben Weg wie Tastatur/Gamepad
 * (hle_hid_set_input), veraendert keinen Gastzustand. */
static PressEv g_press[PRESS_MAX];
static int g_press_n = 0;

static void press_apply(HleHidInput *hi, int frame, unsigned guest) {
    for (int i = 0; i < g_press_n; i++) {
        PressEv *e = &g_press[i];
        if (!press_ev_active(e, frame, guest)) continue;
        if (!strcmp(e->key, "a")) hi->a = 1;
        else if (!strcmp(e->key, "b")) hi->b = 1;
        else if (!strcmp(e->key, "x")) hi->x = 1;
        else if (!strcmp(e->key, "y")) hi->y = 1;
        else if (!strcmp(e->key, "start")) hi->start = 1;
        else if (!strcmp(e->key, "select")) hi->select = 1;
        else if (!strcmp(e->key, "l")) hi->l = 1;
        else if (!strcmp(e->key, "r")) hi->r = 1;
        else if (!strcmp(e->key, "up")) hi->up = 1;
        else if (!strcmp(e->key, "down")) hi->down = 1;
        else if (!strcmp(e->key, "left")) hi->left = 1;
        else if (!strcmp(e->key, "right")) hi->right = 1;
        else if (!strcmp(e->key, "touch")) {
            hi->touch_pressed = 1;
            hi->touch_x = (uint16_t)(e->tx < 0 ? 0 : e->tx > 319 ? 319 : e->tx);
            hi->touch_y = (uint16_t)(e->ty < 0 ? 0 : e->ty > 239 ? 239 : e->ty);
        }
    }
}

/* Lua-Mods (host.*): Maus/Tasten aus dem Fenster. Im deterministischen
 * Modus neutral (keine Maus, keine Taste gedrueckt), wie fuer die
 * Spieleingabe: ein Lauf darf nicht von Fenster oder Tastatur abhaengen. */
static void mods_mouse(MluaMouse *m) {
    m->over_window = win_mouse_touch(&m->x, &m->y, &m->inside_touch, &m->down);
}

/* Startfehler: port.exe hat ohne --console kein Konsolenfenster, deshalb
 * zusaetzlich als Meldungsfenster (nicht in deterministischen Testlaeufen,
 * dort liest das Tool die Ausgabe). */
static int g_quiet_errors = 0;
static int fatal(int rc, const char *msg) {
    printf("%s\n", msg);
    if (!g_quiet_errors) {
        char m[1024];
        snprintf(m, sizeof(m), "%s\n\nLog: %s", msg,
                 log_path()[0] ? log_path() : "(keins)");
        MessageBoxA(NULL, m, "Professor Layton - Port", MB_OK | MB_ICONERROR);
    }
    return rc;
}

static int mods_key_det(const char *name) {
    int v = win_key_state(name);
    return v < 0 ? -1 : 0;
}

int main(int argc, char **argv) {
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    const char *savedata = NULL;
    int frames_limit = -1;
    int guest_limit = -1;
    int shot_every = 0;
    int shot_from = 0;
    int shot_guest = 0;
    int gast_shot_next = 0;
    int renderer_hw = 1;      /* M17: Hardware-Renderer ist Standard */
    int scale = 0;
    int texfilter = -1;       /* -1 = port.ini [display] texture_filter */
    int det = 0;
    int mods_cli = -1;            /* -1 = port.ini [mods] enabled entscheidet */
    const char *mods_dir = NULL;
    int console_cli = -1;         /* -1 = port.ini [debug] console */
    /* Arbeitspaket 7: Ausgabe immer in logs/port-*.log (logtee.h). */
    log_init();
    setvbuf(stdout, NULL, _IONBF, 0);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0) console_cli = 1;
        else if (strcmp(argv[i], "--no-console") == 0) console_cli = 0;
        else if (strcmp(argv[i], "--deterministic") == 0) g_quiet_errors = 1;
    }
    {
        char v[16];
        int on = 0;
        if (win_config_get("debug", "console", v, sizeof(v))) on = atoi(v) != 0;
        if (console_cli >= 0) on = console_cli;
        if (on) log_open_console();
    }
    if (log_path()[0]) printf("[port] Log: %s\n", log_path());
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--console") == 0 || strcmp(argv[i], "--no-console") == 0) continue;
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--shot-every") == 0 && i + 1 < argc) shot_every = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot-from") == 0 && i + 1 < argc) shot_from = atoi(argv[++i]);
        else if (strcmp(argv[i], "--shot-guest") == 0) shot_guest = 1;
        else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames_limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--frames-guest") == 0 && i + 1 < argc) guest_limit = atoi(argv[++i]);
        else if (strcmp(argv[i], "--press") == 0 && i + 1 < argc)
            g_press_n = press_spec_parse(argv[++i], g_press, g_press_n, PRESS_MAX);
        else if (strcmp(argv[i], "--savedata") == 0 && i + 1 < argc) savedata = argv[++i];
        else if (strcmp(argv[i], "--deterministic") == 0) det = 1;
        else if (strcmp(argv[i], "--mods") == 0) mods_cli = 1;
        else if (strcmp(argv[i], "--no-mods") == 0) mods_cli = 0;
        else if (strcmp(argv[i], "--mods-dir") == 0 && i + 1 < argc) mods_dir = argv[++i];
        else if (strcmp(argv[i], "--split-screens") == 0) win_set_split(1);
        else if (strcmp(argv[i], "--debug") == 0) win_set_debug(1);
        else if (strcmp(argv[i], "--no-debug") == 0) win_set_debug(0);
        else if (strcmp(argv[i], "--fps") == 0) win_set_fps_overlay(1);
        else if (strcmp(argv[i], "--no-fps") == 0) win_set_fps_overlay(0);
        else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
            if (scale < 1 || scale > 4) {
                printf("[port] --scale %d ungueltig (1..4), nehme 1\n", scale);
                scale = 1;
            }
        }
        else if (strcmp(argv[i], "--texture-filter") == 0 && i + 1 < argc) {
            const char *f = argv[++i];
            texfilter = hw_texture_filter_parse(f);
            if (texfilter < 0)
                printf("[port] --texture-filter '%s' unbekannt (none, linear, xbrz), nehme none\n", f);
            if (texfilter < 0) texfilter = HW_TEXFILTER_NONE;
        }
        else if (strcmp(argv[i], "--shot-scaled") == 0) g_shot_scaled = 1;
        else if (strcmp(argv[i], "--single-window") == 0) win_set_split(0);
        else if (strcmp(argv[i], "--renderer") == 0 && i + 1 < argc) {
            const char *r = argv[++i];
            if (strcmp(r, "hw") == 0) renderer_hw = 1;
            else if (strcmp(r, "soft") == 0) renderer_hw = 0;
            else
                printf("[port] unbekannter Renderer '%s' (erlaubt: soft, hw)\n", r);
        }
        else romfs = argv[i];
    }
    if (shot != NULL && frames_limit < 0 && guest_limit < 0) frames_limit = 600;

    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) return fatal(2, "kein Speicher");
    {
        FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
        size_t n;
        if (!f) return fatal(2, "out/exefs/code_decomp.bin fehlt (python tools/build.py)");
        n = fread(g_mem + CODE_BASE, 1, CODE_MAX, f);
        fclose(f);
        printf("code: %zu Bytes geladen\n", n);
    }

    arm_cpu_init(&g_cpu, g_mem, MEM_SIZE);
    g_cpu.r[13] = 0x0FFF0000u;
    g_cpu.tpidruro = 0x1FF82000u;
    g_cpu.r[15] = CODE_BASE;
    if (savedata != NULL) {
        hle_set_savedata_dir(savedata);
        printf("[port] Spielstaende: %s\n", savedata);
    }
    hle_set_deterministic(det);
    {
        /* Mods (docs/MODDING.md): standardmaessig aus; port.ini [mods]
         * enabled/dir, --mods/--no-mods/--mods-dir ueberstimmen. */
        static char ini_dir[512];
        char v[32];
        int on = 0;
        if (win_config_get("mods", "enabled", v, sizeof(v))) on = atoi(v) != 0;
        if (mods_cli >= 0) on = mods_cli;
        if (mods_dir == NULL && win_config_get("mods", "dir", ini_dir, sizeof(ini_dir)) && ini_dir[0])
            mods_dir = ini_dir;
        hle_set_mods(on, mods_dir != NULL ? mods_dir : "mods");
        if (on) printf("[port] Mods an: %s\n", mods_dir != NULL ? mods_dir : "mods");
    }
    hle_init(&g_cpu, romfs);
    arm_set_svc_handler(hle_svc);
    if (renderer_hw) gpu_pica_set_render_hw(1);

    if (det) win_set_no_activate(1);
    if (!win_open(FB_W, FB_H, "Professor Layton and the Azran Legacy")) {
        return fatal(3, "Fenster konnte nicht geoeffnet werden");
    }
    /* M17: Ausgabe des HW-Renderers ueber eine D3D11-Swapchain je Fenster,
       interne Aufloesung aus --scale bzw. port.ini [display] scale. Der
       Software-Renderer bleibt bei GDI. */
    if (gpu_pica_render_hw()) {
        if (scale <= 0) scale = win_config_scale();
        hw_set_scale(scale > 0 ? scale : 1);
        /* Texturfilter der internen Aufloesung (nur Anzeige): --texture-filter
           bzw. port.ini [display] texture_filter, Standard none. */
        if (texfilter < 0) {
            char v[32];
            if (win_config_get("display", "texture_filter", v, sizeof(v)) && v[0]) {
                texfilter = hw_texture_filter_parse(v);
                if (texfilter < 0)
                    printf("[port] port.ini texture_filter '%s' unbekannt, nehme none\n", v);
            }
            if (texfilter < 0) texfilter = HW_TEXFILTER_NONE;
        }
        hw_texture_filter_sync(det);   /* reproduzierbare hd-Bilder */
        hw_set_texture_filter(texfilter);
        if (texfilter != HW_TEXFILTER_NONE)
            printf("[port] Texturfilter %s%s\n", hw_texture_filter_name(texfilter),
                   hw_scale() > 1 ? "" : " (wirkt erst ab --scale 2)");
        if (hwp_init(FB_W, FB_H)) win_set_present(present_pane, hwp_detach);
    }
    /* Unterer Screen als Touchflaeche; Tastenbelegung siehe window.h. */
    win_set_touch_rect(BOT_X, BOT_Y, 320, 240);
    /* Titelzeile ohne Diagnose; mit --debug / [debug] overlay = 1 kommen
       fps und Gast-Frames dazu, und das Fenster zeigt die Statuszeilen. */
    win_set_title("Professor Layton and the Azran Legacy");
    win_status("Tasten: port.ini neben port.exe (Standard siehe docs/FRONTEND.md)", 2);
    win_status("F11=Vollbild  Touch=Maus ueber dem unteren Screen", 3);
    {
        MluaHostOps ops;
        ops.mouse = det ? NULL : mods_mouse;
        ops.key = det ? mods_key_det : win_key_state;
        ops.set_cursor = win_set_cursor_mode;
        mlua_set_host_ops(&ops);
    }
    if (!CreateThread(NULL, 0, game_thread, NULL, 0, NULL)) {
        return fatal(3, "Thread fehlgeschlagen");
    }

    int frames = 0;
    unsigned guest_frames = 0;
    FrameClock clock;
    uint64_t fps_t0;
    int fps_frames0 = 0;
    double host_fps = 0.0;
    unsigned fps_guest0 = 0;
    double guest_fps = 0.0;
    frame_clock_start(&clock);
    fps_t0 = frame_clock_now_qpc();
    while (win_pump()) {
        char line[160];
        WinInput wi;
        HleHidInput hi;
        if (det) {
            /* Gleichschritt: warten, bis der Gast am naechsten VBlank steht;
               das Fenster bleibt dabei bedienbar. */
            int got = 0;
            while (!(got = hle_det_wait_vblank(20)) && !g_halt)
                if (!win_pump()) break;
            if (!got) break;
        }
        /* --frames-guest N: Ende am N-ten Gast-Frame (unter Last robust,
           tools/scene.py); --frames bleibt als Host-Grenze bestehen. */
        if ((frames_limit >= 0 && frames >= frames_limit) ||
            (guest_limit >= 0 && (int)hle_gsp_guest_frames() >= guest_limit)) {
            if (shot != NULL) save_shot(shot);
            break;
        }
        /* Host-Eingabe je Frame; der Runtime wendet sie in hid_poll unter dem
           Gast-Lock an (nur Host-Puffer, nie direkter Gastzugriff). */
        win_input_poll(&wi);
        /* Deterministisch: nur --press erreicht den Gast; ein Klick ins
           Testfenster darf den Lauf nicht veraendern. */
        if (det) memset(&wi, 0, sizeof(wi));
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
        press_apply(&hi, frames, hle_gsp_guest_frames());
        hle_hid_set_input(&hi);

        compose_frame();
        win_blit(g_rgb, FB_W, FB_H);
        if (hwp_active()) present_frame();
        if (shot != NULL && shot_every > 0) {
            if (shot_guest) {
                /* Diagnose: Aufnahme an denselben Gast-Frames beider Pfade
                   (die Host-Frame-Zahl ist wegen unterschiedlicher
                   Gastgeschwindigkeit nicht vergleichbar). */
                unsigned g = hle_gsp_guest_frames();
                while (gast_shot_next + shot_every <= (int)g) {
                    gast_shot_next += shot_every;
                    if (gast_shot_next >= shot_from)
                        save_shot_numbered(shot, gast_shot_next);
                }
            } else if (frames > 0 && frames >= shot_from &&
                       (frames % shot_every) == 0) {
                save_shot_numbered(shot, frames);
            }
        }
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
            guest_frames = hle_gsp_guest_frames();
            /* Gast-fps: VBlanks mit neuem Spielbild je Sekunde Wandzeit
               (die Bildrate des Spiels, nicht die des Host-Fensters). */
            if (elapsed > 0) {
                guest_fps = (double)(guest_frames - fps_guest0) *
                            (double)clock.freq / (double)elapsed;
            }
            fps_t0 = now;
            fps_frames0 = frames;
            fps_guest0 = guest_frames;
            snprintf(line, sizeof(line), "%.1f FPS", guest_fps);
            win_status(line, 0);
            snprintf(title, sizeof(title),
                     "Professor Layton and the Azran Legacy  |  fps=%.1f  |  "
                     "Gast-fps=%.1f  |  Gast-Frames=%u  |  F11=Vollbild",
                     host_fps, guest_fps, guest_frames);
            if (win_debug()) win_set_title(title);
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
                /* Nur bei Stillstand (Gast-Zaehler seit der letzten Pruefung
                   unveraendert), sonst wuerde das Log alle 5 s um einen
                   kompletten Dump wachsen. */
                static unsigned stall_last = 0xFFFFFFFFu;
                unsigned gnow = hle_gsp_guest_frames();
                int stalled = (gnow == stall_last);
                stall_last = gnow;
                if (getenv("HLE_STALLDUMP") && stalled) {
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
        if (det) hle_det_release();
        else frame_clock_wait(&clock);
    }
    if (det) hle_det_quit();
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
    if (hwp_active()) {
        unsigned long long sg = 0, sc = 0, tg = 0, tc = 0;
        hwp_stats(&sg, &sc);
        hw_display_stats(&tg, &tc);
        printf("[present] Bildschirme GPU=%llu CPU=%llu, Transfers GPU=%llu CPU=%llu\n",
               sg, sc, tg, tc);
        hwp_shutdown();
    }
    hle_report();
    return 0;
}
