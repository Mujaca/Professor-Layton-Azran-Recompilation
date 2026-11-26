/* Lua-Mods (docs/MODDING.md, Stufe B): eine Lua-VM fuer alle Mods.
 *
 * Generisch: keine Spieladressen, keine Spieldateinamen. Was ein Mod liest
 * oder schreibt, steht nur im Mod selbst (mods/<name>/scripts/main.lua).
 *
 * Ablauf:
 *   mlua_init(dir, mem)     beim Start (hle_init, Gast noch nicht gestartet):
 *                           Mods wie Stufe A finden (Ordner unter dir,
 *                           priority aus mod.ini aufsteigend, Gleichstand
 *                           nach Ordnername), je Mod scripts/main.lua in
 *                           einer eigenen Umgebung ausfuehren, danach alle
 *                           on_load-Funktionen.
 *   mlua_frame(...)         einmal je Gast-VBlank am sicheren Punkt (alle
 *                           Gast-Threads geparkt, Aufrufer haelt den
 *                           Staffelstab): on_frame-Funktionen; liefert die
 *                           Eingabe-Ueberschreibung (input.set) bis zum
 *                           naechsten VBlank.
 *
 * Alle Lua-Aufrufe laufen auf dem jeweiligen Stabhalter (bzw. beim Start auf
 * dem Hauptthread vor dem Gast), nie parallel. Lua laeuft auf dem Host und
 * fuehrt keine Gast-Bloecke aus; die virtuelle Zeit (--deterministic) bleibt
 * unberuehrt.
 *
 * Fehler in einem Skript (auch Budget-Ueberschreitung) werden mit Modname
 * und Traceback geloggt; danach ruft die Runtime keine Funktion dieses Mods
 * mehr auf, das Spiel laeuft weiter. */
#ifndef LUA_HOST_H
#define LUA_HOST_H

#include <stdint.h>

#include "hid_ring.h"

/* Gastspeicher: geprueft (0 = Adresse ungueltig, Lua-Fehler statt Absturz). */
typedef struct {
    void *ctx;
    int (*read)(void *ctx, uint32_t addr, int size, uint32_t *out);
    int (*write)(void *ctx, uint32_t addr, int size, uint32_t val);
} MluaMemOps;

/* Maus in Touchscreen-Pixeln (0..319, 0..239 im Touchscreen; ausserhalb
 * auch negativ/groesser). over_window = Zeiger ueber einem Spielfenster. */
typedef struct {
    int over_window;
    int x, y;
    int inside_touch;
    int down;
} MluaMouse;

enum { MLUA_CURSOR_ARROW = 0, MLUA_CURSOR_HIDDEN = 1, MLUA_CURSOR_CROSSHAIR = 2 };

/* Host-Seite (Fenster). Jeder Zeiger darf NULL sein (neutral: keine Maus,
 * keine Taste). key: -1 = unbekannter Tastenname, sonst 0/1. */
typedef struct {
    void (*mouse)(MluaMouse *out);
    int (*key)(const char *name);
    void (*set_cursor)(int mode);
} MluaHostOps;

/* input.set: Felder, die ein Mod fuer den laufenden VBlank ueberschreibt. */
enum {
    MLUA_IN_A = 1u << 0, MLUA_IN_B = 1u << 1, MLUA_IN_X = 1u << 2, MLUA_IN_Y = 1u << 3,
    MLUA_IN_L = 1u << 4, MLUA_IN_R = 1u << 5, MLUA_IN_START = 1u << 6, MLUA_IN_SELECT = 1u << 7,
    MLUA_IN_UP = 1u << 8, MLUA_IN_DOWN = 1u << 9, MLUA_IN_LEFT = 1u << 10, MLUA_IN_RIGHT = 1u << 11,
    MLUA_IN_CIRCLE_X = 1u << 12, MLUA_IN_CIRCLE_Y = 1u << 13,
    MLUA_IN_TOUCH = 1u << 14, MLUA_IN_TOUCH_X = 1u << 15, MLUA_IN_TOUCH_Y = 1u << 16
};

typedef struct {
    uint32_t mask;
    HidRingInput v;
} MluaInputOverride;

static inline void mlua_apply_override(HidRingInput *in, const MluaInputOverride *ov) {
    uint32_t m = ov->mask;
    if (m == 0) return;
    if (m & MLUA_IN_A) in->a = ov->v.a;
    if (m & MLUA_IN_B) in->b = ov->v.b;
    if (m & MLUA_IN_X) in->x = ov->v.x;
    if (m & MLUA_IN_Y) in->y = ov->v.y;
    if (m & MLUA_IN_L) in->l = ov->v.l;
    if (m & MLUA_IN_R) in->r = ov->v.r;
    if (m & MLUA_IN_START) in->start = ov->v.start;
    if (m & MLUA_IN_SELECT) in->select = ov->v.select;
    if (m & MLUA_IN_UP) in->up = ov->v.up;
    if (m & MLUA_IN_DOWN) in->down = ov->v.down;
    if (m & MLUA_IN_LEFT) in->left = ov->v.left;
    if (m & MLUA_IN_RIGHT) in->right = ov->v.right;
    if (m & MLUA_IN_CIRCLE_X) in->circle_x = ov->v.circle_x;
    if (m & MLUA_IN_CIRCLE_Y) in->circle_y = ov->v.circle_y;
    if (m & MLUA_IN_TOUCH) in->touch_pressed = ov->v.touch_pressed;
    if (m & MLUA_IN_TOUCH_X) in->touch_x = ov->v.touch_x;
    if (m & MLUA_IN_TOUCH_Y) in->touch_y = ov->v.touch_y;
}

/* Liefert die Zahl der Mods mit main.lua (0 = Lua inaktiv). */
int mlua_init(const char *mods_dir, const MluaMemOps *mem);
int mlua_active(void);
void mlua_set_host_ops(const MluaHostOps *ops);
/* Instruktionsbudget je Aufruf (Hauptchunk, on_load, on_frame). */
void mlua_set_budget(uint64_t instructions);
/* base = Eingabe, die das Spiel ohne Mods saehe; ov = Ueberschreibung bis
 * zum naechsten VBlank (mask 0 = keine). */
void mlua_frame(uint32_t guest_frame, uint64_t guest_ns, const HidRingInput *base,
                MluaInputOverride *ov);
void mlua_shutdown(void);

#endif
