/* Lua-Mods (Stufe B), siehe lua_host.h und docs/MODDING.md.
 *
 * Sandbox: geoeffnet werden nur base, string, table, math, utf8 und
 * coroutine (third_party/lua, ohne io/os/debug/package). Aus base entfernt:
 * dofile, loadfile, print (je Mod ersetzt durch log), warn; load nimmt nur
 * Text-Chunks; require laedt nur .lua-Dateien aus scripts/ desselben Mods.
 * math.random ist fest geseedet (math.randomseed() ohne Argument = 0), die
 * String-Hash-Saat ist fest (LUA_DEFINES in tools/build.py). Jeder Mod hat
 * eine eigene Umgebung (globale Variablen getrennt, Lesen faellt auf die
 * gemeinsame Basis zurueck).
 *
 * Grenzen: Instruktionsbudget je Aufruf ueber einen Count-Hook (Fehler, den
 * pcall/xpcall/coroutine.resume nicht abfangen koennen), Speichergrenze im
 * Allokator. */
#include "lua_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"

#define MLUA_MAX_MODS 64
#define MLUA_HOOK_STEP 1000
#define MLUA_MEM_LIMIT ((size_t)256 << 20)

typedef struct {
    char name[128];
    wchar_t *dir;
    int prio;
    int disabled;
    int env_ref;       /* Umgebungstabelle */
    int load_ref;      /* Liste der on_load-Funktionen */
    int frame_ref;     /* Liste der on_frame-Funktionen */
    int loaded_ref;    /* require-Cache */
} LMod;

static struct {
    lua_State *L;
    LMod mods[MLUA_MAX_MODS];
    int nmods;
    int active;
    int phase_load;    /* 1 = Hauptchunks laufen (on_load erlaubt) */
    const MluaMemOps *mem;
    MluaHostOps host;
    uint64_t budget;
    uint64_t used;
    int exhausted;
    size_t mem_used;
    /* Zustand des laufenden VBlanks */
    uint32_t frame;
    uint64_t time_ns;
    uint64_t vblanks;
    HidRingInput base;
    MluaInputOverride *ov;
    MluaInputOverride ov_none;
} g;

/* ------------------------------------------------------------------ */
/* Speicher und Budget                                                 */

static void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
    (void)ud;
    if (ptr == NULL) osize = 0;
    if (nsize == 0) {
        free(ptr);
        g.mem_used -= osize;
        return NULL;
    }
    if (nsize > osize && g.mem_used - osize + nsize > MLUA_MEM_LIMIT) return NULL;
    {
        void *p = realloc(ptr, nsize);
        if (p != NULL) g.mem_used = g.mem_used - osize + nsize;
        return p;
    }
}

static void count_hook(lua_State *L, lua_Debug *ar) {
    (void)ar;
    g.used += MLUA_HOOK_STEP;
    if (g.used > g.budget) {
        g.exhausted = 1;
        luaL_error(L, "Instruktionsbudget (%I) ueberschritten", (lua_Integer)g.budget);
    }
}

void mlua_set_budget(uint64_t instructions) {
    g.budget = instructions ? instructions : 1;
}

/* pcall/xpcall wie lbaselib, aber ein erschoepftes Budget wird weitergereicht. */
static int finish_pcall(lua_State *L, int status, lua_KContext extra) {
    if (status != LUA_OK && status != LUA_YIELD) {
        if (g.exhausted) return lua_error(L);
        lua_pushboolean(L, 0);
        lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - (int)extra;
}

static int sb_pcall(lua_State *L) {
    int status;
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1);
    lua_insert(L, 1);
    status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, finish_pcall);
    return finish_pcall(L, status, 0);
}

static int sb_xpcall(lua_State *L) {
    int status;
    int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushboolean(L, 1);
    lua_pushvalue(L, 1);
    lua_rotate(L, 3, 2);
    status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, finish_pcall);
    return finish_pcall(L, status, 2);
}

/* coroutine.resume faengt Fehler ab; ein erschoepftes Budget nicht. */
static int sb_resume(lua_State *L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, n, LUA_MULTRET);
    if (g.exhausted) return luaL_error(L, "Instruktionsbudget (%I) ueberschritten", (lua_Integer)g.budget);
    return lua_gettop(L);
}

/* math.randomseed() ohne Argument: fester Wert statt Uhrzeit. */
static int sb_randomseed(lua_State *L) {
    int n = lua_gettop(L);
    lua_pushvalue(L, lua_upvalueindex(1));
    if (n == 0) {
        lua_pushinteger(L, 0);
        n = 1;
    } else {
        lua_insert(L, 1);
    }
    lua_call(L, n, 2);
    return 2;
}

/* ------------------------------------------------------------------ */
/* Hilfen                                                              */

static LMod *cur_mod(lua_State *L) {
    int i = (int)lua_tointeger(L, lua_upvalueindex(1));
    return (i >= 0 && i < g.nmods) ? &g.mods[i] : &g.mods[0];
}

static void log_line(const char *mod, const char *text) {
    printf("[mod:%s] %s\n", mod, text);
    fflush(stdout);
}

/* log(...) / print(...): Argumente mit tostring, durch Leerzeichen getrennt. */
static int api_log(lua_State *L) {
    LMod *m = cur_mod(L);
    int n = lua_gettop(L), i;
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (i = 1; i <= n; i++) {
        size_t l;
        const char *s = luaL_tolstring(L, i, &l);
        if (i > 1) luaL_addchar(&b, ' ');
        luaL_addvalue(&b);
        (void)s;
    }
    luaL_pushresult(&b);
    log_line(m->name, lua_tostring(L, -1));
    return 0;
}

static wchar_t *widen(const char *s) {
    UINT cp = CP_UTF8;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0);
    wchar_t *w;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s, -1, NULL, 0);
    }
    w = (wchar_t *)malloc(sizeof(wchar_t) * (size_t)(n > 0 ? n : 1));
    if (w == NULL) return NULL;
    if (n <= 0 || MultiByteToWideChar(cp, 0, s, -1, w, n) <= 0) w[0] = 0;
    return w;
}

/* Datei komplett lesen (NULL = fehlt). */
static char *read_file(const wchar_t *path, size_t *len) {
    FILE *f = _wfopen(path, L"rb");
    char *buf;
    long n;
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) n = 0;
    buf = (char *)malloc((size_t)n + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    *len = fread(buf, 1, (size_t)n, f);
    buf[*len] = 0;
    fclose(f);
    return buf;
}

/* Chunk (Text) laden und als erstes Upvalue die Mod-Umgebung setzen. */
static int load_mod_chunk(lua_State *L, LMod *m, const char *buf, size_t len, const char *chunkname) {
    int st = luaL_loadbufferx(L, buf, len, chunkname, "t");
    if (st != LUA_OK) return st;
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->env_ref);
    if (lua_setupvalue(L, -2, 1) == NULL) lua_pop(L, 1);
    return LUA_OK;
}

/* load(chunk [, name [, mode [, env]]]): nur Strings, nur Text. Ohne env
 * gilt die Umgebung des Mods. */
static int sb_load(lua_State *L) {
    size_t l;
    const char *s = luaL_checklstring(L, 1, &l);
    const char *name = luaL_optstring(L, 2, s);
    int has_env = !lua_isnone(L, 4);
    int st = luaL_loadbufferx(L, s, l, name, "t");
    if (st != LUA_OK) {
        luaL_pushfail(L);
        lua_insert(L, -2);
        return 2;
    }
    if (has_env) lua_pushvalue(L, 4);
    else lua_pushvalue(L, lua_upvalueindex(2));
    if (lua_setupvalue(L, -2, 1) == NULL) lua_pop(L, 1);
    return 1;
}

/* require("name") / require("ordner.name"): scripts/<name>.lua desselben Mods. */
static int sb_require(lua_State *L) {
    LMod *m = cur_mod(L);
    const char *name = luaL_checkstring(L, 1);
    size_t i, n = strlen(name);
    char rel[256], chunk[512];
    wchar_t *wrel, path[1024];
    char *buf;
    size_t len;
    if (n == 0 || n > 200 || name[0] == '.' || name[n - 1] == '.' || strstr(name, "..") != NULL)
        return luaL_error(L, "require: ungueltiger Modulname '%s'", name);
    for (i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-' || c == '.'))
            return luaL_error(L, "require: ungueltiger Modulname '%s'", name);
    }
    lua_rawgeti(L, LUA_REGISTRYINDEX, m->loaded_ref);
    if (lua_getfield(L, -1, name) != LUA_TNIL) return 1;
    lua_pop(L, 1);
    for (i = 0; i <= n; i++) rel[i] = name[i] == '.' ? '\\' : name[i];
    wrel = widen(rel);
    _snwprintf(path, 1024, L"%ls\\scripts\\%ls.lua", m->dir, wrel ? wrel : L"");
    path[1023] = 0;
    free(wrel);
    buf = read_file(path, &len);
    if (buf == NULL) return luaL_error(L, "require: Modul '%s' nicht gefunden (scripts/%s.lua)", name, rel);
    snprintf(chunk, sizeof(chunk), "@%s/scripts/%s.lua", m->name, rel);
    if (load_mod_chunk(L, m, buf, len, chunk) != LUA_OK) {
        free(buf);
        return lua_error(L);
    }
    free(buf);
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }
    lua_pushvalue(L, -1);
    lua_setfield(L, -3, name);
    return 1;
}

/* on_load(fn) / on_frame(fn): an die Liste des Mods anhaengen. */
static int add_cb(lua_State *L, int ref) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    lua_pushvalue(L, 1);
    lua_rawseti(L, -2, (lua_Integer)lua_rawlen(L, -2) + 1);
    return 0;
}

static int api_on_load(lua_State *L) {
    LMod *m = cur_mod(L);
    if (!g.phase_load) return luaL_error(L, "on_load nur im Hauptchunk (main.lua) erlaubt");
    return add_cb(L, m->load_ref);
}

static int api_on_frame(lua_State *L) {
    return add_cb(L, cur_mod(L)->frame_ref);
}

/* ------------------------------------------------------------------ */
/* mem.*                                                               */

/* Upvalues: Groesse, Art (0 unsigned, 1 signed, 2 float). */
static uint32_t check_addr(lua_State *L, int size) {
    lua_Integer a = luaL_checkinteger(L, 1);
    if (a < 0 || a > 0xFFFFFFFFll || (uint64_t)a + (uint64_t)size > 0x100000000ull)
        luaL_error(L, "mem: Adresse %I ausserhalb 0..0xFFFFFFFF", a);
    return (uint32_t)a;
}

/* lua_pushfstring kennt kein %08X: Meldung vorher formatieren. */
static int mem_fail(lua_State *L, const char *what, int size, uint32_t addr) {
    char msg[96];
    snprintf(msg, sizeof(msg), "mem: %s von %d Byte an 0x%08X ungueltig", what, size, (unsigned)addr);
    return luaL_error(L, "%s", msg);
}

static int api_mem_read(lua_State *L) {
    int size = (int)lua_tointeger(L, lua_upvalueindex(1));
    int kind = (int)lua_tointeger(L, lua_upvalueindex(2));
    uint32_t addr = check_addr(L, size), v = 0;
    if (g.mem == NULL || g.mem->read == NULL || !g.mem->read(g.mem->ctx, addr, size, &v))
        return mem_fail(L, "Lesen", size, addr);
    if (kind == 2) {
        float f;
        memcpy(&f, &v, 4);
        lua_pushnumber(L, (lua_Number)f);
    } else if (kind == 1) {
        int64_t s = size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
        lua_pushinteger(L, (lua_Integer)s);
    } else {
        lua_pushinteger(L, (lua_Integer)v);
    }
    return 1;
}

static int api_mem_write(lua_State *L) {
    int size = (int)lua_tointeger(L, lua_upvalueindex(1));
    int kind = (int)lua_tointeger(L, lua_upvalueindex(2));
    uint32_t addr = check_addr(L, size), v;
    if (kind == 2) {
        float f = (float)luaL_checknumber(L, 2);
        memcpy(&v, &f, 4);
    } else {
        lua_Integer x = luaL_checkinteger(L, 2);
        v = (uint32_t)(uint64_t)x;
        if (size == 1) v &= 0xFFu;
        else if (size == 2) v &= 0xFFFFu;
    }
    if (g.mem == NULL || g.mem->write == NULL || !g.mem->write(g.mem->ctx, addr, size, v))
        return mem_fail(L, "Schreiben", size, addr);
    return 0;
}

/* ------------------------------------------------------------------ */
/* input.*                                                             */

typedef struct {
    const char *name;
    uint32_t bit;
    int kind;            /* 0 Taste, 1 Circle, 2 Touch-Koordinate */
    size_t off;
    int lo, hi;
} InField;

#define OFF(f) offsetof(HidRingInput, f)
static const InField IN_FIELDS[] = {
    {"a", MLUA_IN_A, 0, OFF(a), 0, 1}, {"b", MLUA_IN_B, 0, OFF(b), 0, 1},
    {"x", MLUA_IN_X, 0, OFF(x), 0, 1}, {"y", MLUA_IN_Y, 0, OFF(y), 0, 1},
    {"l", MLUA_IN_L, 0, OFF(l), 0, 1}, {"r", MLUA_IN_R, 0, OFF(r), 0, 1},
    {"start", MLUA_IN_START, 0, OFF(start), 0, 1}, {"select", MLUA_IN_SELECT, 0, OFF(select), 0, 1},
    {"up", MLUA_IN_UP, 0, OFF(up), 0, 1}, {"down", MLUA_IN_DOWN, 0, OFF(down), 0, 1},
    {"left", MLUA_IN_LEFT, 0, OFF(left), 0, 1}, {"right", MLUA_IN_RIGHT, 0, OFF(right), 0, 1},
    {"circle_x", MLUA_IN_CIRCLE_X, 1, OFF(circle_x), -0x9C, 0x9C},
    {"circle_y", MLUA_IN_CIRCLE_Y, 1, OFF(circle_y), -0x9C, 0x9C},
    {"touch", MLUA_IN_TOUCH, 3, OFF(touch_pressed), 0, 1},
    {"touch_x", MLUA_IN_TOUCH_X, 2, OFF(touch_x), 0, 319},
    {"touch_y", MLUA_IN_TOUCH_Y, 2, OFF(touch_y), 0, 239},
};
#undef OFF
#define N_IN_FIELDS (sizeof(IN_FIELDS) / sizeof(IN_FIELDS[0]))

static int field_get(const HidRingInput *in, const InField *f) {
    const char *p = (const char *)in + f->off;
    switch (f->kind) {
        case 0: return *(const uint32_t *)p != 0;
        case 1: return *(const int16_t *)p;
        case 2: return *(const uint16_t *)p;
        default: return *(const int *)p != 0;
    }
}

static void field_set(HidRingInput *in, const InField *f, int v) {
    char *p = (char *)in + f->off;
    switch (f->kind) {
        case 0: *(uint32_t *)p = (uint32_t)(v != 0); break;
        case 1: *(int16_t *)p = (int16_t)v; break;
        case 2: *(uint16_t *)p = (uint16_t)v; break;
        default: *(int *)p = v != 0; break;
    }
}

static int api_input_get(lua_State *L) {
    HidRingInput in = g.base;
    size_t i;
    mlua_apply_override(&in, g.ov);
    lua_createtable(L, 0, (int)N_IN_FIELDS);
    for (i = 0; i < N_IN_FIELDS; i++) {
        const InField *f = &IN_FIELDS[i];
        int v = field_get(&in, f);
        if (f->kind == 0 || f->kind == 3) lua_pushboolean(L, v);
        else lua_pushinteger(L, v);
        lua_setfield(L, -2, f->name);
    }
    return 1;
}

/* input.set{a=true, touch=true, touch_x=160, ...}: nur fuer diesen VBlank. */
static int api_input_set(lua_State *L) {
    MluaInputOverride tmp = *g.ov;
    if (g.ov == &g.ov_none) return luaL_error(L, "input.set nur in on_frame");
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, 1) != 0) {
        const char *k = lua_type(L, -2) == LUA_TSTRING ? lua_tostring(L, -2) : NULL;
        const InField *f = NULL;
        size_t i;
        int v;
        for (i = 0; k != NULL && i < N_IN_FIELDS; i++)
            if (strcmp(k, IN_FIELDS[i].name) == 0) f = &IN_FIELDS[i];
        if (f == NULL) return luaL_error(L, "input.set: unbekanntes Feld '%s'", k ? k : "?");
        if (f->kind == 0 || f->kind == 3) {
            if (!lua_isboolean(L, -1)) return luaL_error(L, "input.set: '%s' erwartet true/false", f->name);
            v = lua_toboolean(L, -1);
        } else {
            lua_Integer x;
            if (!lua_isinteger(L, -1)) return luaL_error(L, "input.set: '%s' erwartet eine Ganzzahl", f->name);
            x = lua_tointeger(L, -1);
            v = x < f->lo ? f->lo : x > f->hi ? f->hi : (int)x;
        }
        field_set(&tmp.v, f, v);
        tmp.mask |= f->bit;
        lua_pop(L, 1);
    }
    *g.ov = tmp;
    return 0;
}

/* ------------------------------------------------------------------ */
/* host.*, guest.*                                                     */

static int api_host_mouse(lua_State *L) {
    MluaMouse mm;
    memset(&mm, 0, sizeof(mm));
    mm.x = mm.y = -1;
    if (g.host.mouse != NULL) g.host.mouse(&mm);
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, mm.x);
    lua_setfield(L, -2, "x");
    lua_pushinteger(L, mm.y);
    lua_setfield(L, -2, "y");
    lua_pushboolean(L, mm.inside_touch);
    lua_setfield(L, -2, "inside_touch");
    lua_pushboolean(L, mm.down);
    lua_setfield(L, -2, "down");
    lua_pushboolean(L, mm.over_window);
    lua_setfield(L, -2, "over_window");
    return 1;
}

static int api_host_key(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    int v = 0;
    if (g.host.key != NULL) {
        v = g.host.key(name);
        if (v < 0) return luaL_error(L, "host.key: unbekannte Taste '%s'", name);
    }
    lua_pushboolean(L, v > 0);
    return 1;
}

static int api_host_set_cursor(lua_State *L) {
    static const char *const modes[] = {"arrow", "hidden", "crosshair", NULL};
    int m = luaL_checkoption(L, 1, NULL, modes);
    if (g.host.set_cursor != NULL) g.host.set_cursor(m);
    return 0;
}

static int api_guest_frame(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)g.frame);
    return 1;
}

static int api_guest_time_ns(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)g.time_ns);
    return 1;
}

static int api_guest_vblank(lua_State *L) {
    lua_pushinteger(L, (lua_Integer)g.vblanks);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Aufbau der VM                                                       */

static void set_fn(lua_State *L, const char *name, lua_CFunction f) {
    lua_pushcfunction(L, f);
    lua_setfield(L, -2, name);
}

static void build_api(lua_State *L) {
    static const struct { const char *name; int size, kind; } rd[] = {
        {"read_u8", 1, 0}, {"read_u16", 2, 0}, {"read_u32", 4, 0},
        {"read_s8", 1, 1}, {"read_s16", 2, 1}, {"read_s32", 4, 1}, {"read_f32", 4, 2},
    };
    static const struct { const char *name; int size, kind; } wr[] = {
        {"write_u8", 1, 0}, {"write_u16", 2, 0}, {"write_u32", 4, 0}, {"write_f32", 4, 2},
    };
    size_t i;
    lua_newtable(L);
    for (i = 0; i < sizeof(rd) / sizeof(rd[0]); i++) {
        lua_pushinteger(L, rd[i].size);
        lua_pushinteger(L, rd[i].kind);
        lua_pushcclosure(L, api_mem_read, 2);
        lua_setfield(L, -2, rd[i].name);
    }
    for (i = 0; i < sizeof(wr) / sizeof(wr[0]); i++) {
        lua_pushinteger(L, wr[i].size);
        lua_pushinteger(L, wr[i].kind);
        lua_pushcclosure(L, api_mem_write, 2);
        lua_setfield(L, -2, wr[i].name);
    }
    lua_setglobal(L, "mem");

    lua_newtable(L);
    set_fn(L, "get", api_input_get);
    set_fn(L, "set", api_input_set);
    lua_setglobal(L, "input");

    lua_newtable(L);
    set_fn(L, "mouse", api_host_mouse);
    set_fn(L, "key", api_host_key);
    set_fn(L, "set_cursor", api_host_set_cursor);
    lua_setglobal(L, "host");

    lua_newtable(L);
    set_fn(L, "frame", api_guest_frame);
    set_fn(L, "time_ns", api_guest_time_ns);
    set_fn(L, "vblank", api_guest_vblank);
    lua_setglobal(L, "guest");
}

static void build_sandbox(lua_State *L) {
    static const char *const drop[] = {"dofile", "loadfile", "load", "print", "warn", "require", NULL};
    int i;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");          /* Binaer-Chunks sind ohnehin gesperrt */
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_getfield(L, -1, "resume");
    lua_pushcclosure(L, sb_resume, 1);
    lua_setfield(L, -2, "resume");
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_getfield(L, -1, "randomseed");
    lua_pushvalue(L, -1);
    lua_pushinteger(L, 0);
    lua_call(L, 1, 0);                    /* feste Saat statt Uhrzeit */
    lua_pushcclosure(L, sb_randomseed, 1);
    lua_setfield(L, -2, "randomseed");
    lua_pop(L, 1);

    lua_pushglobaltable(L);
    for (i = 0; drop[i] != NULL; i++) {
        lua_pushnil(L);
        lua_setfield(L, -2, drop[i]);
    }
    set_fn(L, "pcall", sb_pcall);
    set_fn(L, "xpcall", sb_xpcall);
    lua_pop(L, 1);
    build_api(L);
}

/* Eigene Umgebung je Mod: globale Variablen des Mods, Lesen faellt auf die
 * gemeinsame Basis zurueck. */
static void build_env(lua_State *L, int idx) {
    LMod *m = &g.mods[idx];
    lua_newtable(L);                       /* env */
    lua_newtable(L);                       /* Metatabelle */
    lua_pushglobaltable(L);
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "_G");

#define CLOSURE(name, fn)                     \
    lua_pushinteger(L, idx);                  \
    lua_pushcclosure(L, fn, 1);               \
    lua_setfield(L, -2, name)
    CLOSURE("log", api_log);
    CLOSURE("print", api_log);
    CLOSURE("on_load", api_on_load);
    CLOSURE("on_frame", api_on_frame);
    CLOSURE("require", sb_require);
#undef CLOSURE
    lua_pushinteger(L, idx);
    lua_pushvalue(L, -2);                  /* env als Standardumgebung */
    lua_pushcclosure(L, sb_load, 2);
    lua_setfield(L, -2, "load");

    /* mod = {name, priority, on_load, on_frame} */
    lua_newtable(L);
    lua_pushstring(L, m->name);
    lua_setfield(L, -2, "name");
    lua_pushinteger(L, m->prio);
    lua_setfield(L, -2, "priority");
    lua_getfield(L, -2, "on_load");
    lua_setfield(L, -2, "on_load");
    lua_getfield(L, -2, "on_frame");
    lua_setfield(L, -2, "on_frame");
    lua_setfield(L, -2, "mod");

    m->env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    m->load_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    m->frame_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    m->loaded_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}

static int msgh(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    if (msg == NULL) msg = luaL_tolstring(L, 1, NULL);
    luaL_traceback(L, L, msg, 1);
    return 1;
}

static void disable_mod(LMod *m) {
    m->disabled = 1;
    log_line(m->name, "deaktiviert (keine weiteren Aufrufe, das Spiel laeuft weiter)");
}

/* Funktion + nargs Argumente liegen oben; geschuetzt mit Budget aufrufen.
 * 0 = Fehler (geloggt, Mod deaktiviert). */
static int call_mod(LMod *m, int nargs) {
    lua_State *L = g.L;
    int base = lua_gettop(L) - nargs;
    int st;
    lua_pushcfunction(L, msgh);
    lua_insert(L, base);
    g.used = 0;
    g.exhausted = 0;
    st = lua_pcall(L, nargs, 0, base);
    lua_remove(L, base);
    g.exhausted = 0;
    if (st != LUA_OK) {
        const char *e = lua_tostring(L, -1);
        printf("[mod:%s] Fehler: %s\n", m->name, e ? e : "(kein Text)");
        lua_pop(L, 1);
        disable_mod(m);
        return 0;
    }
    return 1;
}

/* Alle Funktionen einer Liste (Registry-Referenz) des Mods aufrufen. */
static void run_list(LMod *m, int ref, int with_frame) {
    lua_State *L = g.L;
    lua_Integer i, n;
    if (m->disabled) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    n = (lua_Integer)lua_rawlen(L, -1);
    for (i = 1; i <= n && !m->disabled; i++) {
        lua_rawgeti(L, -1, i);
        if (with_frame) lua_pushinteger(L, (lua_Integer)g.frame);
        if (!call_mod(m, with_frame ? 1 : 0)) break;
        n = (lua_Integer)lua_rawlen(L, -1);    /* on_frame kann weitere anhaengen */
    }
    lua_pop(L, 1);
}

/* ------------------------------------------------------------------ */
/* Mods finden (wie romfs_overlay.c: Ordner, priority aus mod.ini)     */

static void read_mod_ini(LMod *m) {
    wchar_t p[1024];
    char line[512];
    FILE *f;
    _snwprintf(p, 1024, L"%ls\\mod.ini", m->dir);
    p[1023] = 0;
    f = _wfopen(p, L"rb");
    if (f == NULL) return;
    while (fgets(line, sizeof(line), f)) {
        char *k = line, *v, *e;
        while (*k == ' ' || *k == '\t') k++;
        if (*k == '#' || *k == ';' || *k == '[') continue;
        v = strchr(k, '=');
        if (v == NULL) continue;
        *v++ = 0;
        for (e = v + strlen(v); e > v && (e[-1] == '\r' || e[-1] == '\n' || e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
        while (*v == ' ' || *v == '\t') v++;
        for (e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t');) *--e = 0;
        if (_stricmp(k, "priority") == 0) m->prio = atoi(v);
    }
    fclose(f);
}

static int mod_cmp(const void *a, const void *b) {
    const LMod *x = (const LMod *)a, *y = (const LMod *)b;
    if (x->prio != y->prio) return x->prio < y->prio ? -1 : 1;
    return wcscmp(x->dir, y->dir);
}

static void find_mods(const char *mods_dir) {
    wchar_t *wdir = widen(mods_dir), pat[1024];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (wdir == NULL) return;
    _snwprintf(pat, 1024, L"%ls\\*", wdir);
    pat[1023] = 0;
    h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            wchar_t p[1024];
            LMod *m;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            _snwprintf(p, 1024, L"%ls\\%ls\\scripts\\main.lua", wdir, fd.cFileName);
            p[1023] = 0;
            if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) continue;
            if (g.nmods == MLUA_MAX_MODS) {
                printf("[mods] mehr als %d Lua-Mods, Rest ignoriert\n", MLUA_MAX_MODS);
                break;
            }
            m = &g.mods[g.nmods++];
            memset(m, 0, sizeof(*m));
            _snwprintf(p, 1024, L"%ls\\%ls", wdir, fd.cFileName);
            p[1023] = 0;
            m->dir = _wcsdup(p);
            WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, m->name, (int)sizeof(m->name) - 1, NULL, NULL);
            read_mod_ini(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    free(wdir);
    qsort(g.mods, (size_t)g.nmods, sizeof(LMod), mod_cmp);
}

/* ------------------------------------------------------------------ */
/* Schnittstelle                                                       */

int mlua_init(const char *mods_dir, const MluaMemOps *mem) {
    int i;
    mlua_shutdown();
    if (g.budget == 0) g.budget = 50000000ull;
    g.mem = mem;
    g.ov = &g.ov_none;
    find_mods(mods_dir);
    if (g.nmods == 0) return 0;
    g.L = lua_newstate(l_alloc, NULL);
    if (g.L == NULL) {
        printf("[mods] Lua-VM konnte nicht angelegt werden\n");
        return 0;
    }
    lua_sethook(g.L, count_hook, LUA_MASKCOUNT, MLUA_HOOK_STEP);
    build_sandbox(g.L);
    printf("[mods] Lua: %d Mod(s) mit scripts/main.lua\n", g.nmods);
    g.phase_load = 1;
    for (i = 0; i < g.nmods; i++) {
        LMod *m = &g.mods[i];
        wchar_t p[1024];
        char *buf, chunk[200];
        size_t len = 0;
        build_env(g.L, i);
        printf("[mods]   Lua %s (priority %d)\n", m->name, m->prio);
        _snwprintf(p, 1024, L"%ls\\scripts\\main.lua", m->dir);
        p[1023] = 0;
        buf = read_file(p, &len);
        if (buf == NULL) {
            log_line(m->name, "scripts/main.lua nicht lesbar");
            m->disabled = 1;
            continue;
        }
        snprintf(chunk, sizeof(chunk), "@%s/scripts/main.lua", m->name);
        if (load_mod_chunk(g.L, m, buf, len, chunk) != LUA_OK) {
            printf("[mod:%s] Fehler: %s\n", m->name, lua_tostring(g.L, -1));
            lua_pop(g.L, 1);
            disable_mod(m);
        } else {
            call_mod(m, 0);
        }
        free(buf);
    }
    g.phase_load = 0;
    for (i = 0; i < g.nmods; i++) run_list(&g.mods[i], g.mods[i].load_ref, 0);
    g.active = 1;
    return g.nmods;
}

int mlua_active(void) {
    return g.active;
}

void mlua_set_host_ops(const MluaHostOps *ops) {
    if (ops != NULL) g.host = *ops;
    else memset(&g.host, 0, sizeof(g.host));
}

void mlua_frame(uint32_t guest_frame, uint64_t guest_ns, const HidRingInput *base,
                MluaInputOverride *ov) {
    int i;
    memset(ov, 0, sizeof(*ov));
    if (!g.active) return;
    g.frame = guest_frame;
    g.time_ns = guest_ns;
    g.vblanks++;
    if (base != NULL) g.base = *base;
    else memset(&g.base, 0, sizeof(g.base));
    g.ov = ov;
    for (i = 0; i < g.nmods; i++) run_list(&g.mods[i], g.mods[i].frame_ref, 1);
    g.ov = &g.ov_none;
    lua_settop(g.L, 0);
}

void mlua_shutdown(void) {
    int i;
    if (g.L != NULL) lua_close(g.L);
    for (i = 0; i < g.nmods; i++) free(g.mods[i].dir);
    {
        uint64_t budget = g.budget;
        MluaHostOps host = g.host;
        memset(&g, 0, sizeof(g));
        g.budget = budget;
        g.host = host;
    }
    g.ov = &g.ov_none;
}
