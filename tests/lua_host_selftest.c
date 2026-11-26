/* Testtreiber fuer runtime/modding/lua_host.c (tests/lua_host_test.py).
 *
 *   lua_host_selftest.exe <mods-ordner> <vblanks> [budget]
 *
 * Kein Gast: ein falscher Gastspeicher (0x1000 Byte ab 0x00100000, Inhalt
 * i & 0xFF), feste Host-Zustaende (Maus 12/34 im Touchscreen, Taste F5
 * gedrueckt) und eine synthetische Eingabe je VBlank. Ausgaben "[test] ..."
 * prueft das Python-Skript. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua_host.h"

#define FAKE_BASE 0x00100000u
#define FAKE_SIZE 0x1000u
static uint8_t g_fake[FAKE_SIZE];

static int fake_read(void *ctx, uint32_t addr, int size, uint32_t *out) {
    uint32_t v = 0;
    int i;
    (void)ctx;
    if (addr < FAKE_BASE || (uint64_t)addr + (uint64_t)size > FAKE_BASE + FAKE_SIZE) return 0;
    for (i = size - 1; i >= 0; i--) v = (v << 8) | g_fake[addr - FAKE_BASE + (uint32_t)i];
    *out = v;
    return 1;
}

static int fake_write(void *ctx, uint32_t addr, int size, uint32_t val) {
    int i;
    (void)ctx;
    if (addr < FAKE_BASE || (uint64_t)addr + (uint64_t)size > FAKE_BASE + FAKE_SIZE) return 0;
    for (i = 0; i < size; i++) g_fake[addr - FAKE_BASE + (uint32_t)i] = (uint8_t)(val >> (8 * i));
    return 1;
}

static void fake_mouse(MluaMouse *m) {
    m->over_window = 1;
    m->x = 12;
    m->y = 34;
    m->inside_touch = 1;
    m->down = 0;
}

static int fake_key(const char *name) {
    if (strcmp(name, "F5") == 0) return 1;
    if (strcmp(name, "A") == 0) return 0;
    return -1;
}

static void fake_cursor(int mode) {
    printf("[test] cursor=%d\n", mode);
}

int main(int argc, char **argv) {
    static const MluaMemOps mem = {NULL, fake_read, fake_write};
    MluaHostOps host = {fake_mouse, fake_key, fake_cursor};
    int n, f, i;
    uint32_t sum = 0;
    if (argc < 3) {
        printf("usage: %s <mods> <vblanks> [budget]\n", argv[0]);
        return 2;
    }
    for (i = 0; i < (int)FAKE_SIZE; i++) g_fake[i] = (uint8_t)i;
    if (argc > 3) mlua_set_budget(strtoull(argv[3], NULL, 0));
    mlua_set_host_ops(&host);
    n = mlua_init(argv[1], &mem);
    printf("[test] init=%d active=%d\n", n, mlua_active());
    for (f = 0; f < atoi(argv[2]); f++) {
        HidRingInput base;
        MluaInputOverride ov;
        memset(&base, 0, sizeof(base));
        base.a = (uint32_t)(f & 1);
        base.touch_x = 100;
        mlua_frame((uint32_t)(f / 2), (uint64_t)f * 16666667ull, &base, &ov);
        mlua_apply_override(&base, &ov);
        printf("[test] vbl=%d mask=%05X a=%u b=%u start=%u touch=%d,%u,%u circle=%d,%d\n", f,
               (unsigned)ov.mask, base.a, base.b, base.start, base.touch_pressed, base.touch_x,
               base.touch_y, base.circle_x, base.circle_y);
    }
    for (i = 0; i < (int)FAKE_SIZE; i++) sum = sum * 31u + g_fake[i];
    printf("[test] mem sum=%08X w0=%02X%02X%02X%02X\n", sum, g_fake[3], g_fake[2], g_fake[1], g_fake[0]);
    mlua_shutdown();
    printf("[test] ende\n");
    return 0;
}
