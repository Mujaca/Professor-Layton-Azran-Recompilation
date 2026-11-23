/* Port-Host: fuehrt den recompilierten Code aus und zeigt den Framebuffer. */
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

/* Fenster: 400x480 = oberer Screen (400x240) + unterer Screen (320x240).
   Die 3DS-Framebuffer liegen getrennt; ihre Adressen kommen aus
   gsp::Gpu SetBufferSwap (screen=0/1, Getter hle_gsp_fb_addr). */
#define FB_W 400
#define FB_H 480
#define TOP_W 400
#define TOP_H 240
#define BOT_W 320
#define BOT_H 240
#define BOT_X 40
#define BOT_Y 240

void fn_00100000(ArmCpu *c);

static unsigned char *g_mem = NULL;
static ArmCpu g_cpu;
static int g_halt = 0;
static int g_game_fb_seen = 0;
static unsigned char g_rgb[FB_W * FB_H * 3];
static unsigned char g_official[TOP_W * TOP_H * 3];
static unsigned char g_official_bottom[BOT_W * BOT_H * 3];
static int g_official_ok = 0;
static int g_official_bottom_ok = 0;

static int load_bin(const char *const *paths, unsigned npaths,
                    unsigned char *dst, size_t want) {
    for (unsigned pi = 0; pi < npaths; pi++) {
        FILE *f = fopen(paths[pi], "rb");
        if (!f) continue;
        size_t n = fread(dst, 1, want, f);
        fclose(f);
        if (n == want) return 1;
    }
    return 0;
}

static void load_official_frame(void) {
    if (getenv("PORT_NO_OFFICIAL") != NULL) {
        printf("Referenz-Frame deaktiviert (PORT_NO_OFFICIAL)\n");
        return;
    }
    /* GPU-Pfad: Frame wird als GPU-Kommandoliste (gsp SubmitCmdList/
       TriggerCmdReqQueue/SetBufferSwap) in den VRAM gerendert, nicht per
       Host-memcpy vorab hineinkopiert. */
    if (getenv("HLE_MENU_GPU") != NULL) {
        printf("GPU-Menuepfad aktiv (kein Host-memcpy)\n");
        return;
    }
    const char *top_paths[] = { "out/menu_official.bin", "reports/menu_official.bin" };
    const char *bot_paths[] = { "out/menu_official_bottom.bin", "reports/menu_official_bottom.bin" };
    g_official_ok = load_bin(top_paths, 2, g_official, sizeof(g_official));
    g_official_bottom_ok = load_bin(bot_paths, 2, g_official_bottom, sizeof(g_official_bottom));
    if (g_official_ok)
        printf("Top-Referenz geladen (nur Vergleich)\n");
    if (g_official_bottom_ok)
        printf("Bottom-Referenz geladen (nur Vergleich)\n");
}

static DWORD WINAPI game_thread(LPVOID unused) {
    (void)unused;
    fn_00100000(&g_cpu);
    g_halt = 1;
    return 0;
}

static void draw_icon(void);

static const char *MENU_ITEMS[] = { "NEW GAME", "CONTINUE", "OPTIONS" };
static int g_menu_sel = 0;
static char g_menu_msg[64] = "";

/* --- Minimaler 5x7-Font fuer den Menue-Platzhalter --- */
static const unsigned char FONT5x7[][5] = {
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0,0,0,0,0}, {0x08,0x08,0x08,0x08,0x08}, {0,0x36,0x36,0,0},
    {0,0,0x5F,0,0}, {0,0x60,0x60,0,0}
};

static int font_index(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == ' ') return 36;
    if (c == '-') return 37;
    if (c == ':') return 38;
    if (c == '!') return 39;
    if (c == '.') return 40;
    return 36;
}

static void draw_text(int x0, int y0, const char *txt, int scale, int r, int g, int b) {
    int x = x0;
    for (const char *p = txt; *p; p++) {
        int idx = font_index(*p);
        for (int col = 0; col < 5; col++) {
            unsigned char bits = FONT5x7[idx][col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) {
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) {
                            int px = x + col * scale + dx, py = y0 + row * scale + dy;
                            if (px < 0 || px >= FB_W || py < 0 || py >= FB_H) continue;
                            unsigned char *d = g_rgb + ((size_t)py * FB_W + px) * 3;
                            d[0] = (unsigned char)r; d[1] = (unsigned char)g; d[2] = (unsigned char)b;
                        }
                }
            }
        }
        x += 6 * scale;
    }
}

static void draw_menu(void) {
    for (int y = 0; y < TOP_H; y++) {
        int v = 10 + y * 50 / TOP_H;
        for (int x = 0; x < TOP_W; x++) {
            unsigned char *d = g_rgb + ((size_t)y * FB_W + x) * 3;
            d[0] = (unsigned char)(v / 2);
            d[1] = (unsigned char)(v / 2);
            d[2] = (unsigned char)v;
        }
    }
    draw_icon();
    draw_text((TOP_W - 16 * 6 * 2) / 2, 12, "PROFESSOR LAYTON", 2, 245, 220, 120);
    draw_text((TOP_W - 19 * 6) / 2, 34, "AND THE AZRAN LEGACY", 1, 200, 200, 210);
    for (int i = 0; i < 3; i++) {
        int y = 96 + i * 22;
        int sel = (i == g_menu_sel);
        int w = (int)strlen(MENU_ITEMS[i]) * 6 * 2;
        int x = (TOP_W - w) / 2;
        draw_text(x, y, MENU_ITEMS[i], 2,
                  sel ? 255 : 170, sel ? 230 : 170, sel ? 120 : 170);
        if (sel) {
            draw_text(x - 16, y, ">", 2, 255, 230, 120);
            draw_text(x + w + 4, y, "<", 2, 255, 230, 120);
        }
    }
    if (g_menu_msg[0])
        draw_text((TOP_W - (int)strlen(g_menu_msg) * 6) / 2, TOP_H - 34, g_menu_msg,
                  1, 255, 255, 255);
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
    int ox = (TOP_W - 48 * scale) / 2;
    int oy = (TOP_H - 48 * scale) / 2;

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

/* Kopiert einen RGB24-Rechteckbereich aus dem Gast-VRAM in das Fensterbild. */
static void copy_fb_region(int dx, int dy, int w, int h, uint32_t addr) {
    uint32_t off = addr & 0x1FFFFFFFu;
    size_t bytes = (size_t)w * (size_t)h * 3u;
    if (off + bytes > MEM_SIZE) return;
    const unsigned char *src = g_mem + off;
    for (int y = 0; y < h; y++) {
        memcpy(g_rgb + ((size_t)(dy + y) * FB_W + dx) * 3,
               src + (size_t)y * w * 3, (size_t)w * 3);
    }
}

/* Beide Screens direkt aus den vom Spiel gesetzten Framebuffern
   zusammensetzen (kein Referenzbild als Renderquelle). */
static void compose_fb(void) {
    memset(g_rgb, 0, sizeof(g_rgb));
    copy_fb_region(0, 0, TOP_W, TOP_H, hle_gsp_fb_addr(0));
    copy_fb_region(BOT_X, BOT_Y, BOT_W, BOT_H, hle_gsp_fb_addr(1));
}

/* Nur Debug-Fallback: Referenzframes als Pixelvergleichsbild. */
static void compose_official(void) {
    memset(g_rgb, 0, sizeof(g_rgb));
    if (g_official_ok)
        memcpy(g_rgb, g_official, sizeof(g_official));
    if (g_official_bottom_ok) {
        for (int y = 0; y < BOT_H; y++)
            memcpy(g_rgb + ((size_t)(BOT_Y + y) * FB_W + BOT_X) * 3,
                   g_official_bottom + (size_t)y * BOT_W * 3, (size_t)BOT_W * 3);
    }
}

/* Bildquelle des Fensters (Engine-Framebuffer, Referenz oder Menue). */
static int compose_frame(void) {
    int fb_mode = (getenv("HLE_ENGINE_FRAME") != NULL) ||
                  (getenv("HLE_MENU_GPU") != NULL) ||
                  (getenv("PORT_FB_MODE") != NULL);
    if (fb_mode) compose_fb();
    else if (g_official_ok) compose_official();
    else draw_menu();
    return fb_mode;
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
    /* Standard: die Spiel-Engine rendert selbst (fn_0028F084). Der
       Referenz-/GPU-Blit bleibt nur als Debug-Fallback ueber
       HLE_MENU_GPU=1 bzw. PORT_NO_ENGINE=1 erreichbar. */
    if (getenv("HLE_ENGINE_FRAME") == NULL && getenv("PORT_NO_ENGINE") == NULL) {
        _putenv_s("HLE_ENGINE_FRAME", "1");
    }
    /* Echte PICA-Kommandolisten der Engine ausfuehren (2D-Teilmenge). */
    if (getenv("HLE_ENGINE_FRAME") != NULL && getenv("HLE_PICA_RENDER") == NULL) {
        _putenv_s("HLE_PICA_RENDER", "1");
    }
    /* Engine-eigenen 2D-Scene-Renderer (0x1ED8FC) takten. */
    if (getenv("HLE_ENGINE_FRAME") != NULL && getenv("HLE_ENGINE_2D") == NULL) {
        _putenv_s("HLE_ENGINE_2D", "1");
    }
    const char *romfs = "out/romfs";
    const char *shot = NULL;
    int keyseq[32];
    int keyn = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shot") == 0 && i + 1 < argc) shot = argv[++i];
        else if (strcmp(argv[i], "--keys") == 0 && i + 1 < argc) {
            char *tok = argv[++i];
            while (*tok && keyn < 32) {
                char name[16];
                int n = 0;
                while (*tok && *tok != ',' && n < 15) name[n++] = *tok++;
                name[n] = 0;
                if (*tok == ',') tok++;
                if (strcmp(name, "down") == 0) keyseq[keyn++] = VK_DOWN;
                else if (strcmp(name, "up") == 0) keyseq[keyn++] = VK_UP;
                else if (strcmp(name, "enter") == 0) keyseq[keyn++] = VK_RETURN;
            }
        }
        else romfs = argv[i];
    }

    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) { printf("kein Speicher\n"); return 2; }

    FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
    if (!f) { printf("code_decomp.bin fehlt\n"); return 2; }
    size_t n = fread(g_mem + 0x100000, 1, 0x300000, f);
    fclose(f);
    printf("code: %zu Bytes geladen\n", n);
    /* Gezielte Code-Patches (Diagnose): SDK-Fehlerpfade entschaerfen.
     * 0x212CC0: "bleq 0x28F45C" (wirft FS-Fehler, wenn ein Session-Handle 0 ist)
     *           -> NOP, damit der Aufruf ohne Abbruch weiterlaeuft. */
    if (0x212CC0u + 4 <= 0x300000u) {
        *(uint32_t *)(g_mem + 0x212CC0) = 0xE1A00000u;
        *(uint32_t *)(g_mem + 0x212FCC) = 0xE1A00000u;
        /* 0x33E1B0: "str r0,[r4]" im Session-Destruktor -> NOP,
           damit das statische Session-Objekt nie Handle 0 bekommt. */
        *(uint32_t *)(g_mem + 0x33E1B0) = 0xE1A00000u;
        /* 0x107A48 / 0x107B34: zwei SDK-Fehlerpfade (F96047A2,
           C8A04554) entschaerfen. */
        *(uint32_t *)(g_mem + 0x107A48) = 0xE1A00000u;
        *(uint32_t *)(g_mem + 0x107B34) = 0xE1A00000u;
        /* 0x28F45C = SDK-Fehlerwerfer: komplett entschaerfen (bx lr). */
        *(uint32_t *)(g_mem + 0x28F45C) = 0xE12FFF1Eu;
        /* 0x28EE00 (srvGetServiceHandle-Wrapper) hat zwei Guards, die den
           Request ohne Sendung abbrechen: [0x3D92EC] <= 0 und name_len > 8.
           Beide Pruefungen entschaerfen, damit der Request rausgeht. */
        for (unsigned q = 0; q < 6; q++)
            *(uint32_t *)(g_mem + 0x28EE08u + q * 4) = 0xE1A00000u;
        /* 0x11A818: kalibrierte Busy-Wait-Delay -> sofort zurueck,
           sonst blockiert der Delay den kooperativen Scheduler. */
        *(uint32_t *)(g_mem + 0x11A818) = 0xE3A00000u;  /* mov r0,#0 */
        *(uint32_t *)(g_mem + 0x11A81C) = 0xE12FFF1Eu;  /* bx lr */
        /* Engine-SetBufferSwap (fn_001196AC): der VBlank-Wait 0x2877C0
           blockiert, weil der Port keine GSP-IRQ-Relay-Pulse liefert.
           Der Submit selbst (0x120CFC -> gsp 0x0005) laeuft weiter. */
        if (getenv("HLE_ENGINE_FRAME") != NULL) {
            /* fn_001196AC komplett als Erfolg zurueckgeben: der VBlank-Wait
               haengt ohne echte GSP-IRQ-Pulse. Der Renderer submitted seine
               eigene Liste danach ueber 0x1B9938/0x34xxxx. */
            *(uint32_t *)(g_mem + 0x1196AC) = 0xE3A00001u;  /* mov r0,#1 */
            *(uint32_t *)(g_mem + 0x1196B0) = 0xE12FFF1Eu;  /* bx lr */
        }
        /* Diagnose: Scene-Readiness 0x37EF94 (+0x38) meldet fertig, damit
           fn_00103D20 die Warteschleife (0x103DA8 -> 0x28F1E8) verlaesst. */
        if (getenv("HLE_ENGINE_DRAW") != NULL) {
            *(uint32_t *)(g_mem + 0x37EF94) = 0xE3A00000u;  /* mov r0,#0 */
            *(uint32_t *)(g_mem + 0x37EF98) = 0xE12FFF1Eu;  /* bx lr */
            /* Readiness-Loop am 0x103DA8 direkt verlassen: beq -> b 0x103DC8 */
            *(uint32_t *)(g_mem + 0x103DA4) = 0xEA000007u;
        }
    }


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
    load_official_frame();
    {
        int fb_mode = (getenv("HLE_ENGINE_FRAME") != NULL) ||
                      (getenv("HLE_MENU_GPU") != NULL) ||
                      (getenv("PORT_FB_MODE") != NULL);
        if (fb_mode) compose_fb();
        else if (g_official_ok) compose_official();
        else draw_menu();
        if (fb_mode) win_status("Engine-Framebuffer (oben+unten)", 2);
        else if (g_official_ok) win_status("Referenz-Frame (nur Debug)", 2);
    }

    HANDLE th = CreateThread(NULL, 0, game_thread, NULL, 0, NULL);
    if (!th) { printf("Thread fehlgeschlagen\n"); return 3; }

    int frames = 0;
    int shot_saved = 0;
    while (win_pump()) {
        if (frames >= 2 && frames - 2 < keyn) win_key_push(keyseq[frames - 2]);
        if (win_key_take(VK_DOWN)) { g_menu_sel = (g_menu_sel + 1) % 3; g_menu_msg[0] = 0; }
        if (win_key_take(VK_UP))   { g_menu_sel = (g_menu_sel + 2) % 3; g_menu_msg[0] = 0; }
        if (win_key_take(VK_RETURN)) {
            snprintf(g_menu_msg, sizeof(g_menu_msg), "%s SELECTED", MENU_ITEMS[g_menu_sel]);
        }
        {
            int fb_mode = compose_frame();
            if (fb_mode && !g_game_fb_seen) {
                g_game_fb_seen = 1;
                win_status("game framebuffer active (screen 0+1)", 2);
            }
        }
        win_blit(g_rgb, FB_W, FB_H);

        static int tick = 0;
        if ((++tick % 30) == 0) {
            char line[192];
            uint32_t units = 0, dla = 0, dl0 = 0, pool = 0;
            uint16_t dlc = 0, mn = 0, mcap = 0, pstart = 0, pend = 0;
            memcpy(&units, g_mem + 0x00422200u + 0x1530u, 4);
            memcpy(&dlc, g_mem + 0x003EE1A0u, 2);
            memcpy(&mcap, g_mem + 0x003EE1A2u, 2);
            memcpy(&mn, g_mem + 0x003EE196u, 2);
            memcpy(&dla, g_mem + 0x003EE198u, 4);
            memcpy(&pool, g_mem + 0x003EE188u, 4);
            memcpy(&pstart, g_mem + 0x003EE190u, 2);
            memcpy(&pend, g_mem + 0x003EE194u, 2);
            if (dla != 0u) {
                uint32_t base = dla & 0x1FFFFFFFu;
                memcpy(&dl0, g_mem + base, 4);
            }
            printf("[port] frames=%d pc=%08X units=%u dl=%u/%u arr=%08X e0=%08X mn=%u pool=%08X %u..%u\n",
                   frames, g_cpu.r[15], units, (unsigned)dlc, (unsigned)mcap,
                   dla, dl0, (unsigned)mn, pool, (unsigned)pstart, (unsigned)pend);
            {
                /* Diagnose: Gast-RAM nach einem 32-Bit-Wert scannen. */
                static int sp_on = -1, sp_done = 0;
                if (sp_on < 0) sp_on = (getenv("HLE_SCANPTR") != NULL);
                if (sp_on && !sp_done) {
                    const char *sv = getenv("HLE_SCANPTR");
                    const char *sf = getenv("HLE_SCAN_AT");
                    int at = (sf != NULL && *sf != 0) ? atoi(sf) : 200;
                    if (frames >= at && sv != NULL && *sv != 0) {
                        uint32_t want = (uint32_t)strtoul(sv, NULL, 16);
                        uint32_t a, hits = 0;
                        sp_done = 1;
                        printf("[scanptr] want=%08X hits:", want);
                        for (a = 0x00100000u; a + 4u < MEM_SIZE && hits < 16u; a += 4u) {
                            uint32_t v;
                            memcpy(&v, g_mem + a, 4);
                            if (v == want) {
                                printf(" %08X", a);
                                hits++;
                            }
                        }
                        printf("\n");
                        fflush(stdout);
                    }
                }
            }
            {
                /* Diagnose: rohen GSP-/GX-Shared-Memory-Bereich dumpen. */
                static int sd = -1, sd_done = 0;
                if (sd < 0) sd = (getenv("HLE_SHM_DUMP") != NULL);
                if (sd && !sd_done) {
                    const char *sf = getenv("HLE_SHM_AT");
                    int at = (sf != NULL && *sf != 0) ? atoi(sf) : 40;
                    if (frames >= at) {
                        uint32_t a, lo = 0x100027C0u, hi = 0x10002900u;
                        const char *da = getenv("HLE_DUMP_ADDR");
                        const char *dl = getenv("HLE_DUMP_LEN");
                        if (da != NULL && *da != 0) {
                            lo = (uint32_t)strtoul(da, NULL, 16);
                            hi = lo + ((dl != NULL && *dl != 0)
                                       ? (uint32_t)strtoul(dl, NULL, 16) : 0x100u);
                        }
                        sd_done = 1;
                        printf("[shmdump] %08X..%08X:\n", lo, hi);
                        for (a = lo; a < hi; a += 0x10u) {
                            printf("  %08X:", a);
                            for (int k = 0; k < 4; k++) {
                                uint32_t v = 0;
                                memcpy(&v, g_mem + a + (uint32_t)k * 4u, 4);
                                printf(" %08X", v);
                            }
                            printf("\n");
                        }
                    }
                }
            }
            {
                /* Diagnose: GSP-CommandBuffer (SubmitCmdList-Queue) im
                   gesamten Gast-RAM suchen. */
                static int cq = -1, cq_done = 0;                if (cq < 0) cq = (getenv("HLE_SCAN_CMDQ") != NULL);
                if (cq && !cq_done) {
                    const char *sf = getenv("HLE_SCAN_AT");
                    int at = (sf != NULL && *sf != 0) ? atoi(sf) : 120;
                    if (frames >= at) {
                        uint32_t base, found = 0, k;
                        cq_done = 1;
                        printf("[cmdq] shm10000800:");
                        for (k = 0; k < 8u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x10000800u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n[cmdq] shm10002800:");
                        for (k = 0; k < 8u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x10002800u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n[cmdq] scan:");
                        for (base = 0x08000000u; base < 0x20000000u && found < 8u; base += 0x200u) {
                            uint32_t hdr, n, idx, cb, cmd, addr, size;
                            memcpy(&hdr, g_mem + base, 4);
                            n = (hdr >> 8) & 0xFFu;
                            idx = hdr & 0xFFu;
                            if (n == 0u || n > 15u || idx > 14u) continue;
                            cb = base + 0x20u + idx * 0x20u;
                            memcpy(&cmd, g_mem + cb, 4);
                            memcpy(&addr, g_mem + cb + 4u, 4);
                            memcpy(&size, g_mem + cb + 8u, 4);
                            if ((cmd & 0xFFu) != 1u) continue;
                            if (addr < 0x00100000u || addr >= 0x20000000u) continue;
                            if (size == 0u || size > 0x00100000u) continue;
                            printf(" base=%08X hdr=%08X addr=%08X size=%X", base, hdr, addr, size);
                            found++;
                        }
                        printf("\n");
                        fflush(stdout);
                    }
                }
            }
            {
                static int mdump = -1, mdone = 0;
                static int dump_at = -1;
                if (mdump < 0) mdump = (getenv("HLE_MENU_DUMP") != NULL);
                if (dump_at < 0) {
                    const char *da = getenv("HLE_DUMP_AT");
                    dump_at = (da != NULL && *da != 0) ? atoi(da) : 90;
                }
                if (mdump && !mdone && frames >= dump_at) {
                    uint32_t k, vt = 0, sp = 0;
                    uint16_t sc = 0;
                    mdone = 1;
                    memcpy(&vt, g_mem + 0x003EE12Cu, 4);
                    memcpy(&sp, g_mem + 0x003EE194u, 4);
                    memcpy(&sc, g_mem + 0x003EE19Cu, 2);
                    printf("[menuobj] frames=%d vt=%08X seqtab=%08X seqcnt=%u\n",
                           frames, vt, sp, (unsigned)sc);
                    printf("[menuobj] raw3EE12C:");
                    for (k = 0; k < 0x20u; k++) {
                        uint32_t w = 0;
                        memcpy(&w, g_mem + 0x003EE12Cu + k * 4u, 4);
                        printf(" %08X", w);
                    }
                    printf("\n");
                    if (sp >= 0x00100000u && sp < 0x20000000u && sc > 0u) {
                        uint32_t n = sc > 12u ? 12u : sc;
                        printf("[menuobj] seq:");
                        for (k = 0; k < n; k++) {
                            uint32_t w0 = 0, w1 = 0;
                            memcpy(&w0, g_mem + (sp & 0x1FFFFFFFu) + k * 8u, 4);
                            memcpy(&w1, g_mem + (sp & 0x1FFFFFFFu) + k * 8u + 4u, 4);
                            printf(" [%u:%08X/%08X]", (unsigned)k, w0, w1);
                        }
                        printf("\n");
                    }
                    /* Ersten Draw-List-Eintrag (menu+0x6C[0]) und dessen
                       Objekt-Kopf dumpen. */
                    {
                        uint32_t arr2 = 0, obj2 = 0;
                        uint16_t cnt2 = 0;
                        memcpy(&arr2, g_mem + 0x003EE198u, 4);
                        memcpy(&cnt2, g_mem + 0x003EE1A0u, 2);
                        printf("[menuobj] arr=%08X cnt=%u:", arr2,
                               (unsigned)cnt2);
                        if (arr2 >= 0x00100000u && arr2 < 0x20000000u) {
                            uint32_t lim = cnt2 > 16u ? 16u : cnt2;
                            for (k = 0; k < lim; k++) {
                                uint32_t v = 0;
                                memcpy(&v, g_mem + (arr2 & 0x1FFFFFFFu) + k * 4u, 4);
                                printf(" [%u]=%08X", (unsigned)k, v);
                                if (k == 0u) obj2 = v;
                            }
                        }
                        printf("\n");
                        if (arr2 >= 0x00100000u && arr2 < 0x20000000u) {
                            uint32_t lim = cnt2 > 16u ? 16u : cnt2;
                            for (k = 0; k < lim; k++) {
                                uint32_t o = 0, j;
                                memcpy(&o, g_mem + (arr2 & 0x1FFFFFFFu) + k * 4u, 4);
                                if (o < 0x00100000u || o >= 0x20000000u) continue;
                                printf("[menuobj] obj[%u]=%08X:", (unsigned)k, o);
                                for (j = 0; j < 12u; j++) {
                                    uint32_t v = 0;
                                    memcpy(&v, g_mem + (o & 0x1FFFFFFFu) + j * 4u, 4);
                                    printf(" %08X", v);
                                }
                                printf("\n");
                            }
                        }
                        if (obj2 >= 0x00100000u && obj2 < 0x20000000u) {
                            printf("[menuobj] obj%08X:", obj2);
                            for (k = 0; k < 8u; k++) {
                                uint32_t v = 0;
                                memcpy(&v, g_mem + (obj2 & 0x1FFFFFFFu) + k * 4u, 4);
                                printf(" %08X", v);
                            }
                            printf("\n");
                        }
                        /* GSP-Client-Zustand 0x3D7C40: enthaelt er eine
                           Shared-Memory-Adresse? */
                        printf("[menuobj] gspstate:");
                        for (k = 0; k < 0x10u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x003D91E8u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n[menuobj] gspglobals:");
                        for (k = 0; k < 0x10u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x003DA6D0u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n[menuobj] gsplock:");
                        for (k = 0; k < 8u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x0040C620u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n");
                        /* 3D-Unit-Node (HLE_UNIT_XFER) und Renderer-Kopf. */
                        printf("[menuobj] unitnode:");
                        for (k = 0; k < 0x10u; k++) {
                            uint32_t v = 0;
                            memcpy(&v, g_mem + 0x004236F0u + k * 4u, 4);
                            printf(" %08X", v);
                        }
                        printf("\n");
                        {
                            uint32_t uc = 0, u0 = 0;
                            memcpy(&uc, g_mem + 0x00422200u + 0x1530u, 4);
                            memcpy(&u0, g_mem + 0x00422200u + 0x14F0u, 4);
                            printf("[menuobj] units=%u u0=%08X:", uc, u0);
                            if (u0 >= 0x00100000u && u0 < 0x20000000u) {
                                for (k = 0; k < 0x10u; k++) {
                                    uint32_t v = 0;
                                    memcpy(&v, g_mem + (u0 & 0x1FFFFFFFu) + k * 4u, 4);
                                    printf(" %08X", v);
                                }
                            }
                            printf("\n");
                        }
                    }
                    fflush(stdout);
                }
            }
            snprintf(line, sizeof(line), "PC=0x%08X  halted=%d  frames=%d",
                     g_cpu.r[15], g_halt, frames);
            win_status(line, 1);
        }
        frames++;
        Sleep(16);
        {
            static int shot_at = -1, shot_stop = -1;
            static int min_lists = -1, stable_need = -1, stable = 0;
            static unsigned char prev[FB_W * FB_H * 3];
            if (shot_at < 0) {
                const char *sf = getenv("PORT_SHOT_FRAMES");
                if (sf != NULL && *sf != 0) shot_at = atoi(sf);
                else if (getenv("HLE_MENU_GPU") != NULL) shot_at = 240;
                else if (getenv("HLE_ENGINE_FRAME") != NULL) shot_at = 16000;
                else shot_at = 3;
                if (shot_at < 3) shot_at = 3;
                shot_stop = shot_at + 180;
                {
                    const char *sl = getenv("PORT_SHOT_LISTS");
                    min_lists = (sl != NULL && *sl != 0) ? atoi(sl) : 1;
                }
                {
                    const char *ss = getenv("PORT_SHOT_STABLE");
                    stable_need = (ss != NULL && *ss != 0) ? atoi(ss) : 2;
                    if (stable_need < 1) stable_need = 1;
                }
            }
            if (shot != NULL && !shot_saved) {
                int fb_mode = compose_frame();
                unsigned long long lists = gpu_pica_list_count();
                unsigned long long draws = gpu_pica_draw_count();
                if (memcmp(prev, g_rgb, sizeof(prev)) == 0) stable++;
                else { memcpy(prev, g_rgb, sizeof(prev)); stable = 0; }
                if (fb_mode) {
                    if (frames >= shot_at && stable >= stable_need &&
                        lists >= (unsigned long long)min_lists) {
                        printf("[shot] frames=%d lists=%llu draws=%llu "
                               "stable=%d\n", frames, lists, draws, stable);
                        fflush(stdout);
                        save_bmp(shot);
                        shot_saved = 1;
                    }
                } else if (frames == shot_at) {
                    save_bmp(shot);
                    shot_saved = 1;
                }
            }
            if (shot != NULL && shot_saved) break;
            if (shot != NULL && frames > shot_stop) break;
        }
        if (g_halt && shot == NULL) {
            /* Fenster bleibt offen, bis es geschlossen wird */
        }
    }
    if (shot && !shot_saved) save_bmp(shot);
    win_close();
    hle_report();
    return 0;
}
