/* Trace-Runtime: schreibt Records im Format "CTRTRC01" (siehe tools/recomp/trace.py). */
#include "arm_trace.h"
#include "arm_cpu.h"

#include <stdio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

uint32_t arm_last_block = 0;
void (*arm_pump_hook)(void) = NULL;
void (*arm_tick_hook)(struct ArmCpu *c) = NULL;
uint32_t arm_watch_pc = 0;
static uint32_t g_mwk_r0 = 0, g_mwk_r1 = 0;
static int g_watch_hits = 0;

/* ------------------------------------------------------------------ *
 * perfmap: Block-Histogramm pro Thread/Funktion und Rueckwaertskanten.
 * Nur aktiv mit HLE_PERFMAP=1 oder HLE_PERFMAP_FILE=<pfad>. Kein
 * printf pro Block; der Speicher wird periodisch als Textdatei
 * geschrieben und einmal pro Dump als Marker ausgegeben.
 * ------------------------------------------------------------------ */
#define PM_SLOTS (1u << 20)
#define PM_EDGES 1024
typedef struct {
    uint32_t key;      /* pc ^ (thread << 28), 0 = frei */
    uint32_t pad;
    uint64_t count;
} PmSlot;
typedef struct {
    uint32_t from, to;
    uint64_t count;
} PmEdge;
static PmSlot *g_pm_slots = NULL;
static PmEdge g_pm_edges[16][PM_EDGES];
static uint64_t g_pm_total = 0;
static uint64_t g_pm_thread_total[16];
static uint32_t g_pm_last_pc[16];
static int g_pm_on = 0;
static unsigned g_pm_dump_ms = 2000;
static char g_pm_path[512];
static uint64_t g_pm_last_dump_ms = 0;
static unsigned g_pm_dumps = 0;

/* ------------------------------------------------------------------ *
 * hotpatch: Adresse -> C-Callback, registrierbar per
 *   HLE_HOOK=28F284=count_fields,134818=log
 * Gueltige Callback-Namen: count_fields/count, regs/dump_regs,
 * mem/dump_mem, log/pc, set_r0 (mit HLE_HOOK_VALUE=<hex>).
 * ------------------------------------------------------------------ */
#define HOOK_MAX 16
enum { HK_COUNT = 0, HK_REGS, HK_MEM, HK_LOG, HK_SET_R0 };
typedef struct {
    uint32_t addr;
    int kind;
    uint32_t max;
    uint64_t hits;
    char name[32];
} HookEntry;
static HookEntry g_hooks[HOOK_MAX];
static int g_hook_count = 0;
static int g_hooks_on = 0;
static uint32_t g_hook_value = 0;

void arm_perfmap_report(void);
void arm_hook_report(void);
static void perfmap_sample(ArmCpu *c, uint32_t pc);
static void hook_hit(ArmCpu *c, uint32_t pc);
static void perfmap_init(void);
static void hook_init(void);

/* Ringpuffer der zuletzt ausgefuehrten Bloecke (Diagnose bei Abbruch). */
#define PC_RING 65536
static uint32_t g_pc_ring[PC_RING];
static uint32_t g_lr_ring[PC_RING];
static unsigned g_pc_pos = 0;
uint32_t arm_thread_last_pc[16];
static uint32_t g_t_ring[16][64];
static uint32_t g_t_ring_lr[16][64];
static uint32_t g_t_pos[16];

void arm_trace_thread_dump(int ti, unsigned n) {
    if (ti < 0 || ti > 15) return;
    if (n > 64) n = 64;
    fprintf(stderr, "[tdump] thread %X letzte %u Bloecke:\n", ti, n);
    for (unsigned i = 0; i < n; i++) {
        uint32_t idx = (g_t_pos[ti] + 64 - n + i) & 63u;
        fprintf(stderr, "[tdump]   %02u pc=%08X lr=%08X\n", i, g_t_ring[ti][idx], g_t_ring_lr[ti][idx]);
    }
    fflush(stderr);
}

uint32_t arm_thread_last_lr[16];

void arm_trace_pc_ring_dump(unsigned count) {
    unsigned n = count > PC_RING ? PC_RING : count;
    fprintf(stderr, "[ring] letzte %u Bloecke (neueste zuerst):\n", n);
    for (unsigned i = 0; i < n; i++) {
        unsigned idx = (g_pc_pos + PC_RING - 1u - i) & (PC_RING - 1);
        fprintf(stderr, "[ring] %04u pc=%08X lr=%08X\n", i, g_pc_ring[idx], g_lr_ring[idx]);
    }
    fflush(stderr);
}

static FILE *g_file = NULL;
static unsigned long long g_records = 0;
static unsigned int g_flush_counter = 0;
static unsigned long long g_max_records = 20000000ULL;
static int g_unique_only = 1;

/* Offene Adressierung: jede Blockadresse nur einmal schreiben. */
#define SEEN_SLOTS (1u << 21)
static unsigned int *g_seen = NULL;

static int mark_seen(unsigned int pc) {
    unsigned int h = (pc * 2654435761u) & (SEEN_SLOTS - 1);
    for (unsigned int i = 0; i < SEEN_SLOTS; i++) {
        unsigned int slot = (h + i) & (SEEN_SLOTS - 1);
        if (g_seen[slot] == 0) {
            g_seen[slot] = pc + 1;
            return 1;
        }
        if (g_seen[slot] == pc + 1) {
            return 0;
        }
    }
    return 0;
}

static void put_u8(unsigned v) {
    unsigned char b = (unsigned char)(v & 0xFF);
    if (g_file) fwrite(&b, 1, 1, g_file);
}

static void put_u16(unsigned v) {
    put_u8(v);
    put_u8(v >> 8);
}

static void put_u32(unsigned v) {
    put_u8(v);
    put_u8(v >> 8);
    put_u8(v >> 16);
    put_u8(v >> 24);
}

void arm_trace_open(const char *path) {
    const char *all = getenv("ARM_TRACE_ALL");
    const char *max = getenv("ARM_TRACE_MAX");
    if (g_file) return;
    g_unique_only = (all == NULL || *all == '\0' || *all == '0');
    if (max != NULL) {
        g_max_records = strtoull(max, NULL, 0);
    }
    g_records = 0;
    g_seen = (unsigned int *)calloc(SEEN_SLOTS, sizeof(unsigned int));
    g_file = fopen(path ? path : "trace.ctrt", "wb");
    if (g_file) {
        fwrite("CTRTRC01", 1, 8, g_file);
        fflush(g_file);
    }
}

void arm_trace_close(void) {
    if (g_file) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }
    if (g_seen) {
        free(g_seen);
        g_seen = NULL;
    }
}

int arm_trace_is_open(void) {
    return g_file != NULL;
}

/* ---------------- perfmap ---------------- */

static uint32_t pm_hash(uint32_t key)
{
    return (key * 2654435761u) & (PM_SLOTS - 1u);
}

static void perfmap_init(void)
{
    const char *p;
    if (g_pm_on) return;
    p = getenv("HLE_PERFMAP_FILE");
    if (p != NULL && *p != 0)
        snprintf(g_pm_path, sizeof(g_pm_path), "%s", p);
    else
        snprintf(g_pm_path, sizeof(g_pm_path), "out/perfmap.txt");
    p = getenv("HLE_PERFMAP_DUMP_MS");
    if (p != NULL && *p != 0) g_pm_dump_ms = (unsigned)strtoul(p, NULL, 0);
    if (g_pm_dump_ms < 100) g_pm_dump_ms = 100;
    g_pm_slots = (PmSlot *)calloc(PM_SLOTS, sizeof(PmSlot));
    if (g_pm_slots == NULL) return;
    memset(g_pm_edges, 0, sizeof(g_pm_edges));
    memset(g_pm_thread_total, 0, sizeof(g_pm_thread_total));
    memset(g_pm_last_pc, 0, sizeof(g_pm_last_pc));
    g_pm_total = 0;
    g_pm_on = 1;
    g_pm_last_dump_ms = (uint64_t)GetTickCount64();
}

static void perfmap_sample(ArmCpu *c, uint32_t pc)
{
    static unsigned poll = 0;
    uint32_t ti = (uint32_t)((c->tpidruro >> 12) & 0xFu);
    uint32_t key = pc ^ (ti << 28);
    uint32_t h = pm_hash(key);
    uint32_t last;
    int inserted = 0;
    if (!g_pm_on || g_pm_slots == NULL) return;

    for (unsigned i = 0; i < 32u; i++) {
        uint32_t slot = (h + i) & (PM_SLOTS - 1u);
        if (g_pm_slots[slot].key == 0) {
            g_pm_slots[slot].key = key;
            g_pm_slots[slot].count = 1;
            inserted = 1;
            break;
        }
        if (g_pm_slots[slot].key == key) {
            g_pm_slots[slot].count++;
            inserted = 1;
            break;
        }
    }
    (void)inserted;
    g_pm_total++;
    g_pm_thread_total[ti]++;

    /* Rueckwaertskante = pc springt innerhalb von 64 KiB rueckwaerts.
       Das ist die billigste, verlaessliche Spin-Loop-Erkennung. */
    last = g_pm_last_pc[ti];
    if (last != 0 && pc < last && (last - pc) < 0x10000u) {
        uint32_t eh = ((last * 2654435761u) ^ (pc * 2246822519u)) & (PM_EDGES - 1u);
        for (unsigned i = 0; i < 16u; i++) {
            PmEdge *e = &g_pm_edges[ti][(eh + i) & (PM_EDGES - 1u)];
            if (e->count == 0) {
                e->from = last;
                e->to = pc;
                e->count = 1;
                break;
            }
            if (e->from == last && e->to == pc) {
                e->count++;
                break;
            }
        }
    }
    g_pm_last_pc[ti] = pc;

    if ((++poll & 0x1FFFu) == 0) {
        uint64_t now = (uint64_t)GetTickCount64();
        if (now - g_pm_last_dump_ms >= (uint64_t)g_pm_dump_ms) {
            g_pm_last_dump_ms = now;
            g_pm_dumps++;
            {
                FILE *f = fopen(g_pm_path, "w");
                if (f != NULL) {
                    unsigned entries = 0, edges = 0;
                    fprintf(f, "PERFMAP 1\n");
                    fprintf(f, "# total %llu\n",
                            (unsigned long long)g_pm_total);
                    for (int t = 0; t < 16; t++)
                        fprintf(f, "# thread %d %llu\n", t,
                                (unsigned long long)g_pm_thread_total[t]);
                    for (int t = 0; t < 16; t++)
                        for (int i = 0; i < PM_EDGES; i++)
                            if (g_pm_edges[t][i].count != 0) {
                                fprintf(f, "# back %d %08X %08X %llu\n", t,
                                        g_pm_edges[t][i].from,
                                        g_pm_edges[t][i].to,
                                        (unsigned long long)g_pm_edges[t][i].count);
                                edges++;
                            }
                    for (unsigned i = 0; i < PM_SLOTS; i++) {
                        if (g_pm_slots[i].key == 0) continue;
                        fprintf(f, "pc %08X %u %llu\n",
                                g_pm_slots[i].key & 0x0FFFFFFFu,
                                g_pm_slots[i].key >> 28,
                                (unsigned long long)g_pm_slots[i].count);
                        entries++;
                    }
                    fclose(f);
                    printf("[perfmap] dump #%u entries=%u edges=%u total=%llu\n",
                           g_pm_dumps, entries, edges,
                           (unsigned long long)g_pm_total);
                    fflush(stdout);
                }
            }
        }
    }
}

void arm_perfmap_report(void)
{
    if (!g_pm_on) return;
    /* Letzten Stand sicher schreiben (ohne Marker-Rauschen). */
    {
        FILE *f = fopen(g_pm_path, "w");
        if (f != NULL) {
            unsigned entries = 0, edges = 0;
            fprintf(f, "PERFMAP 1\n");
            fprintf(f, "# total %llu\n", (unsigned long long)g_pm_total);
            for (int t = 0; t < 16; t++)
                fprintf(f, "# thread %d %llu\n", t,
                        (unsigned long long)g_pm_thread_total[t]);
            for (int t = 0; t < 16; t++)
                for (int i = 0; i < PM_EDGES; i++)
                    if (g_pm_edges[t][i].count != 0) {
                        fprintf(f, "# back %d %08X %08X %llu\n", t,
                                g_pm_edges[t][i].from, g_pm_edges[t][i].to,
                                (unsigned long long)g_pm_edges[t][i].count);
                        edges++;
                    }
            for (unsigned i = 0; i < PM_SLOTS; i++) {
                if (g_pm_slots[i].key == 0) continue;
                fprintf(f, "pc %08X %u %llu\n",
                        g_pm_slots[i].key & 0x0FFFFFFFu,
                        g_pm_slots[i].key >> 28,
                        (unsigned long long)g_pm_slots[i].count);
                entries++;
            }
            fclose(f);
            printf("[perfmap] report entries=%u edges=%u total=%llu\n",
                   entries, edges, (unsigned long long)g_pm_total);
            fflush(stdout);
        }
    }
    if (g_pm_slots != NULL) {
        free(g_pm_slots);
        g_pm_slots = NULL;
    }
    g_pm_on = 0;
}

/* ---------------- hotpatch ---------------- */

static int hook_kind(const char *name)
{
    if (strcmp(name, "count_fields") == 0 || strcmp(name, "count") == 0)
        return HK_COUNT;
    if (strcmp(name, "regs") == 0 || strcmp(name, "dump_regs") == 0)
        return HK_REGS;
    if (strcmp(name, "mem") == 0 || strcmp(name, "dump_mem") == 0)
        return HK_MEM;
    if (strcmp(name, "log") == 0 || strcmp(name, "pc") == 0)
        return HK_LOG;
    if (strcmp(name, "set_r0") == 0)
        return HK_SET_R0;
    return -1;
}

static void hook_init(void)
{
    const char *spec = getenv("HLE_HOOK");
    const char *mx = getenv("HLE_HOOK_MAX");
    const char *val = getenv("HLE_HOOK_VALUE");
    char buf[512];
    char *tok;
    uint32_t maxhits = 20;
    if (g_hooks_on || spec == NULL || *spec == 0) return;
    if (mx != NULL && *mx != 0) maxhits = (uint32_t)strtoul(mx, NULL, 0);
    if (val != NULL && *val != 0) g_hook_value = (uint32_t)strtoul(val, NULL, 16);
    snprintf(buf, sizeof(buf), "%s", spec);
    for (tok = strtok(buf, ",;"); tok != NULL; tok = strtok(NULL, ",;")) {
        char *eq = strchr(tok, '=');
        int kind;
        if (eq == NULL || g_hook_count >= HOOK_MAX) continue;
        *eq = 0;
        kind = hook_kind(eq + 1);
        if (kind < 0) {
            fprintf(stderr, "[hook] unbekannter Callback: %s\n", eq + 1);
            continue;
        }
        g_hooks[g_hook_count].addr = (uint32_t)strtoul(tok, NULL, 16);
        g_hooks[g_hook_count].kind = kind;
        g_hooks[g_hook_count].max = maxhits;
        g_hooks[g_hook_count].hits = 0;
        snprintf(g_hooks[g_hook_count].name,
                 sizeof(g_hooks[g_hook_count].name), "%s", eq + 1);
        g_hook_count++;
    }
    if (g_hook_count > 0) {
        g_hooks_on = 1;
        printf("[hook] %d Adresse(n) registriert (HLE_HOOK)\n", g_hook_count);
        fflush(stdout);
    }
}

static void hook_hit(ArmCpu *c, uint32_t pc)
{
    for (int i = 0; i < g_hook_count; i++) {
        HookEntry *h = &g_hooks[i];
        if (h->addr != pc || c == NULL) continue;
        h->hits++;
        switch (h->kind) {
        case HK_COUNT:
            if (h->hits == 1 || h->hits == 2 || h->hits == 4 ||
                h->hits == 8 || (h->hits % 1000u) == 0) {
                printf("[hook] %s addr=%08X hits=%llu\n", h->name, pc,
                       (unsigned long long)h->hits);
                fflush(stdout);
            }
            break;
        case HK_REGS:
            if (h->hits <= h->max) {
                printf("[hook] %s addr=%08X hits=%llu r0=%08X r1=%08X r2=%08X "
                       "r3=%08X r4=%08X sp=%08X lr=%08X\n", h->name, pc,
                       (unsigned long long)h->hits, c->r[0], c->r[1], c->r[2],
                       c->r[3], c->r[4], c->r[13], c->r[14]);
                fflush(stdout);
            }
            break;
        case HK_MEM:
            if (h->hits <= h->max) {
                uint32_t a = c->r[0] & ~3u;
                printf("[hook] %s addr=%08X hits=%llu mem@%08X:", h->name,
                       pc, (unsigned long long)h->hits, a);
                for (int k = 0; k < 8; k++)
                    printf(" %08X", (unsigned)arm_mem_read(c, a + (uint32_t)k * 4u, 4));
                printf("\n");
                fflush(stdout);
            }
            break;
        case HK_LOG:
            if (h->hits <= h->max) {
                printf("[hook] %s addr=%08X hits=%llu lr=%08X r0=%08X r1=%08X\n",
                       h->name, pc, (unsigned long long)h->hits, c->r[14],
                       c->r[0], c->r[1]);
                fflush(stdout);
            }
            break;
        case HK_SET_R0:
            c->r[0] = g_hook_value;
            if (h->hits <= h->max) {
                printf("[hook] %s addr=%08X hits=%llu -> r0=%08X\n", h->name,
                       pc, (unsigned long long)h->hits, g_hook_value);
                fflush(stdout);
            }
            break;
        default:
            break;
        }
    }
}

void arm_hook_report(void)
{
    for (int i = 0; i < g_hook_count; i++) {
        if (g_hooks[i].hits == 0) continue;
        printf("[hook-final] %s addr=%08X hits=%llu\n", g_hooks[i].name,
               g_hooks[i].addr, (unsigned long long)g_hooks[i].hits);
        fflush(stdout);
    }
}

/* Env-Flags einmal cachen: getenv() pro Block war der Hauptbremsklotz
   (7 Lookups je ausgefuehrtem Block). */
typedef struct {
    int init;
    int fa_debug, reg_enable, reg_debug, asset_trace, queue_log;
    int fa_reg_ok, fa_fake_ok, fa_always, blklogf, force_asset;
    int dump_fatal, dump_r3, watch_pool, perf, rm_adv, romfs_log, romfs_force, heap_seed;
    int fast_memcpy, fake_asset, load_task, scene_ready;
    int perfmap, hooks;
    uint32_t blklog_lo;
} TraceEnv;
static TraceEnv g_te;

static int envset(const char *n) { return getenv(n) != NULL; }

static void trace_env_init(void) {
    const char *v;
    memset(&g_te, 0, sizeof(g_te));
    g_te.init = 1;
    g_te.fa_debug = envset("HLE_FA_DEBUG");
    g_te.reg_enable = envset("HLE_REG_ENABLE");
    g_te.reg_debug = envset("HLE_REG_DEBUG");
    g_te.asset_trace = envset("HLE_ASSET_TRACE");
    g_te.queue_log = envset("HLE_QUEUE_LOG");
    g_te.fa_reg_ok = envset("HLE_FA_REG_OK");
    g_te.fa_fake_ok = envset("HLE_FA_FAKE_OK");
    g_te.fa_always = envset("HLE_FA_ALWAYS");
    g_te.force_asset = envset("HLE_FORCE_ASSET");
    g_te.dump_fatal = envset("HLE_DUMP_FATAL");
    g_te.dump_r3 = envset("HLE_DUMP_R3");
    g_te.watch_pool = envset("ARM_WATCH_POOL");
    g_te.perf = envset("HLE_PERF");
    g_te.rm_adv = envset("HLE_RM_ADV");
    g_te.romfs_log = envset("HLE_ROMFS_LOG");
    g_te.romfs_force = envset("HLE_ROMFS_FORCE");
    {
        const char *hs = getenv("HLE_HEAP_SEED");
        g_te.heap_seed = !(hs != NULL && hs[0] == '0');
    }
    {
        const char *fm = getenv("HLE_FAST_MEMCPY");
        g_te.fast_memcpy = !(fm != NULL && fm[0] == '0');
    }
    g_te.fake_asset = envset("HLE_FAKE_ASSET");
    g_te.scene_ready = envset("HLE_SCENE_READY");
    {
        const char *lt = getenv("HLE_LOAD_TASK");
        g_te.load_task = (lt != NULL && lt[0] != '0');
    }
    g_te.perfmap = envset("HLE_PERFMAP") || (getenv("HLE_PERFMAP_FILE") != NULL);
    g_te.hooks = (getenv("HLE_HOOK") != NULL);
    if (g_te.perfmap) perfmap_init();
    if (g_te.hooks) hook_init();
    v = getenv("HLE_BLKLOGF");
    if (v != NULL && *v != 0) {
        g_te.blklogf = 1;
        g_te.blklog_lo = (uint32_t)strtoul(v, NULL, 16);
    }
}

void arm_trace_block(ArmCpu *c, uint32_t pc, uint32_t thumb) {
    if (!g_te.init) trace_env_init();
    arm_last_block = pc;
    if (g_te.perfmap && c != NULL) perfmap_sample(c, pc);
    if (g_te.hooks && c != NULL) hook_hit(c, pc);
    if (g_te.perf) {
        static unsigned long long nb = 0, next = 1000000ULL;
        static unsigned long long t0 = 0;
        nb++;
        if (t0 == 0) t0 = (unsigned long long)GetTickCount64();
        if (nb >= next) {
            unsigned long long now = (unsigned long long)GetTickCount64();
            printf("[perf] %llu blocks in %llu ms (%.0f/s)\n", nb,
                   now - t0, (double)nb * 1000.0 / (double)(now - t0));
            fflush(stdout);
            next += 1000000ULL;
        }
    }
    /* Kalibrierte SDK-Busy-Wait-Verzoegerung sofort beenden: ohne echte
       Tick-Zaehlung spinnt sie zig-tausend Runden und blockiert den
       kooperativen Scheduler (Thread 2 blieb dort haengen). */
    if (c != NULL && pc == 0x0011A818u) c->r[0] = 0;
    if (c != NULL && pc == 0x0022ADE4u && g_te.fa_debug) {
        static int nl2 = 0;
        if (nl2 < 8) {
            nl2++;
            printf("[falevel2] hash=%08X base=%08X cnd=%08X\n",
                   c->r[11], c->r[8], c->r[4]);
            fflush(stdout);
        }
    }
    if (c != NULL && pc == 0x0024FD74u && g_te.fa_debug) {
        static int nfamax = 0;
        if (nfamax < 12) {
            nfamax++;
            printf("[fahash] ns=%08X hash=%08X\n", c->r[1], c->r[0]);
            fflush(stdout);
        }
    }

    /* Asset-Registry synthetisch bestuecken: Deskriptor {basis,stride,count}
       plus Eintraege. Layout laut 0x2B95F8: [+0x14] != 0 (aktiv),
       [+0x1C] = Zeiger auf den Dateinamen. */
    if (c != NULL && g_te.reg_enable &&
        (pc == 0x00104E44u || pc == 0x0036C54Cu)) {
        static const uint32_t names[4] = { 0x001015F8u, 0x00101608u,
                                           0x00101618u, 0x00101628u };
        uint32_t d0 = 0x00400000u, d1 = 0x000000C0u, d2 = 4u, g = 0x00401000u;
        uint32_t one = 1u;
        /* Beim Resolver-Aufruf (0x36C54C) zeigt r1 auf den gesuchten Namen.
           Alle Eintraege bekommen diesen Zeiger, damit der Namensvergleich
           (Pointer-Gleichheit in 0x22AE54) garantiert erfolgreich ist. */
        uint32_t name_ptrs[4];
        name_ptrs[0] = names[0]; name_ptrs[1] = names[1];
        name_ptrs[2] = names[2]; name_ptrs[3] = names[3];
        if (pc == 0x0036C54Cu && c->r[1] >= 0x00100000u && c->r[1] < 0x20000000u) {
            name_ptrs[0] = name_ptrs[1] = name_ptrs[2] = name_ptrs[3] = c->r[1];
        }
        for (int i = 0; i < 4; i++) {
            uint32_t e = 0x00400000u + (uint32_t)i * 0xC0u;
            uint32_t ns = 0x00403000u;
            memcpy(c->mem + e + 0x14u, &ns, 4);
            memcpy(c->mem + e + 0x1Cu, &name_ptrs[i], 4);
        }
        /* Fake-Namespace (2 Ebenen) fuer 0x24FD74:
           ns 0x403000 {+4=0x100 Eintraege, +12=0x200 Kindtabelle, +24=1}
           Eintrag 0x403100 {Hash(arc)=0x10AAF679, +8=0, +10=1}
           Kind    0x403200 {Hash(lt6_a.fa)=0xD56041E4} */
        {
            uint32_t z = 0, eoff = 0x100u, ctab = 0x200u, cnt = 1u;
            uint32_t hash_arc = 0x10AAF679u, hash_fa = 0x2A7A446Cu;
            uint16_t cidx = 0u, ccnt = 1u;
            memcpy(c->mem + 0x00403000u, &z, 4);
            memcpy(c->mem + 0x00403004u, &eoff, 4);
            memcpy(c->mem + 0x0040300Cu, &ctab, 4);
            memcpy(c->mem + 0x00403018u, &cnt, 2);
            memcpy(c->mem + 0x00403100u, &hash_arc, 4);
            memcpy(c->mem + 0x00403108u, &cidx, 2);
            memcpy(c->mem + 0x0040310Au, &ccnt, 2);
            memcpy(c->mem + 0x00403200u, &hash_fa, 4);
        }
        memcpy(c->mem + 0x00401000u, &d0, 4);
        memcpy(c->mem + 0x00401004u, &d1, 4);
        memcpy(c->mem + 0x00401008u, &d2, 4);
        memcpy(c->mem + 0x3ECA5Cu, &g, 4);
        if (g_te.reg_debug) {
            printf("[regfill] pc=%08X desc=%08X cnt=%u name0=%08X\n",
                   pc, d0, d2, names[0]);
            fflush(stdout);
        }
    }
    /* Diagnose: beim FA-Scan den Request-Struct zeigen, den 0x2B95F8 parst. */
    if (c != NULL && pc == 0x002B9664u && g_te.reg_debug) {
        static int dbg = 0;
        if (dbg < 3) {
            dbg++;
            printf("[fastruct] entry=%08X off20=%08X req=%08X:",
                   c->r[4], arm_mem_read(c, c->r[4] + 0x20u, 4), c->r[5]);
            for (int i = 0; i < 8; i++)
                printf(" %08X", arm_mem_read(c, c->r[5] + (uint32_t)i * 4u, 4));
            printf("\n");
            fflush(stdout);
        }
    }
    /* Diagnose: Asset-Kette (Registrierung -> Dispatcher) mitzaehlen. */
    if (c != NULL && g_te.asset_trace) {
        if (pc == 0x001013F0u || pc == 0x001014D4u || pc == 0x00291A88u ||
            pc == 0x002919DCu || pc == 0x00291AA0u) {
            printf("[asset-trace] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X\n",
                   pc, c->r[0], c->r[1], c->r[2], c->r[3]);
            fflush(stdout);
        }
    }

    /* Diagnose: Resource-Manager-Zaehler weiterschalten, damit die
       Warteschleife fn_0028F250 (Zustand 0x400) verlaesst. */
    if (c != NULL && g_te.rm_adv && pc == 0x0028F284u) {
        uint32_t v = 0;
        memcpy(&v, c->mem + 0x0040B4C4u, 4);
        v++;
        memcpy(c->mem + 0x0040B4C4u, &v, 4);
    }

    /* Diagnose: RomFS-Mount als erfolgreich behandeln (fn_0011B98C liefert 0). */
    if (c != NULL && g_te.romfs_force && pc == 0x00284D64u) c->r[0] = 1;

    /* Diagnose: RomFS-Archive-Mount fn_00284D24 (schreibt den Guard
       [archive+0xA34] bei 0x285038). */
    /* Diagnose: Heap-Anforderung des Loaders auf 8 MB reduzieren - er
       fordert 0x00FF0000 (16,7 MB) an, der Heap ist aber nur 16 MB gross. */
    if (c != NULL && g_te.heap_seed && pc == 0x0012BC58u) {
        uint32_t sz = 0x00800000u;      /* Heap-Anforderung: 8 MB */
        uint32_t wr = 0x00600000u;      /* _M_Wrk-Objekt: 6 MB (passt in 8 MB) */
        memcpy(c->mem + ((c->r[1] + 0xF0u) & 0x1FFFFFFFu), &sz, 4);
        memcpy(c->mem + ((c->r[1] + 0xF4u) & 0x1FFFFFFFu), &wr, 4);
    }

    /* Diagnose: Lade-Auftrag vortaeuschen, damit fn_001213E8 den Enqueue
       (0x2879B4) aufruft: [obj+0x1D]=1 (Flag) und [obj+0x20]=1 (Anzahl). */
    if (c != NULL && g_te.load_task && pc == 0x001215C0u) {
        uint32_t one = 1u;
        uint32_t o = c->r[4] & 0x1FFFFFFFu;
        c->mem[o + 0x1Du] = 1;
        memcpy(c->mem + o + 0x20u, &one, 4);
        /* Das Ziel-Objekt 0x0042BB10 muss "bereit" sein, sonst bricht
           fn_002879B4 bei [obj+0x1F6]==0 sofort ab (gemessen: Feld ist 0). */
        c->mem[0x0042BB10u + 0x1F6u] = 1;
        printf("[loadtask] obj=%08X -> f1d=1 cnt=1 ready=1\n", c->r[4]);
        fflush(stdout);
    }

    /* Diagnose: Guard-Byte [0x3E7771] vor fn_001213E8 loeschen - es
       verhindert sonst das Einstellen des Lade-Tasks (fn_002879B4). */
    if (c != NULL && g_te.load_task && pc == 0x001213E8u) {
        c->mem[0x003E7771u] = 0;
    }

    /* Diagnose: Frame-Budget des Resource-Managers (0x40B4E8) ueberschreiben. */
    if (c != NULL && pc == 0x00278374u) {
        const char *fb = getenv("HLE_FRAME_BUDGET");
        if (fb != NULL && *fb != 0) {
            c->r[0] = (uint32_t)strtoul(fb, NULL, 0);
            c->r[1] = 0;
            printf("[budget] set to %08X\n", c->r[0]);
            fflush(stdout);
        }
    }

    /* Diagnose: Szene-/Ladezustand als fertig markieren (Objekt 0x3EC028+0x2144). */
    if (c != NULL && g_te.scene_ready && pc == 0x00101508u) {
        uint16_t one = 1;
        memcpy(c->mem + (0x003EE16Cu & 0x1FFFFFFFu), &one, 2);
    }

    /* Diagnose: Registerzustand im Archiv-Dateilader. */
    if (c != NULL && g_te.queue_log &&
        (pc == 0x0026F130u || pc == 0x002808A8u || pc == 0x00280914u)) {
        printf("[clob] pc=%08X r6=%08X r8=%08X\n", pc, c->r[6], c->r[8]);
        fflush(stdout);
    }
    if (c != NULL && g_te.queue_log && (pc == 0x0027A750u || pc == 0x0027A6E8u)) {
        printf("[pop] pc=%08X r6=%08X r8=%08X sp=%08X", pc, c->r[6], c->r[8], c->r[13]);
        for (int q = 0; q < 8; q++) {
            uint32_t v = 0;
            memcpy(&v, c->mem + ((c->r[13] + 0xC4u + (uint32_t)q * 4u) & 0x1FFFFFFFu), 4);
            printf(" %08X", v);
        }
        printf("\n");
        fflush(stdout);
    }
    if (c != NULL && pc == 0x0012493Cu) {
        g_mwk_r0 = c->r[0];
        g_mwk_r1 = c->r[1];
    }
    if (c != NULL && pc == 0x00124980u) {
        if (getenv("HLE_MWK_FIX")) {
            c->r[6] = g_mwk_r0;
            c->r[8] = g_mwk_r1;
        }
        if (g_te.romfs_log) {
            printf("[mwk] r6=%08X r8=%08X sp=%08X lr=%08X\n",
                   c->r[6], c->r[8], c->r[13], c->r[14]);
            fflush(stdout);
        }
    }

    /* Diagnose: Dem Scene-Descriptor einen synthetischen Asset-Eintrag geben,
       damit die Einfuege-Schleife in fn_00284D24 laeuft und 0x282364 einen
       Namen eintraegt. Eintrag: stride 0x18, Name[0x14] @+0, u32 @+0x10,
       u8 @+0x16. */
    if (c != NULL && g_te.fake_asset && pc == 0x00284D80u) {
        uint32_t sc = c->r[7];
        uint32_t cnt = 1u, name = 0x0E000000u, sz = 0x1000u;
        if (sc >= 0x00100000u && sc < 0x20000000u) {
            uint32_t b = sc & 0x1FFFFFFFu;
            memcpy(c->mem + b + 0x104u, &cnt, 4);   /* Anzahl          */
            memcpy(c->mem + b + 0x108u, &name, 4);  /* Eintrag+0: Name */
            c->mem[b + 0x114u] = 0;                 /* +0x14 (s8)      */
            memcpy(c->mem + b + 0x118u, &sz, 4);    /* +0x10: Groesse  */
            c->mem[b + 0x11Du] = 0;                 /* +0x15 (s8)      */
            c->mem[b + 0x11Eu] = 1;                 /* +0x16 (u8)      */
            memset(c->mem + name, 0, 0x20);
            memcpy(c->mem + name, "fix.xr", 7);
            printf("[fake-asset] scene=%08X count=1 name@%08X size=%X\n", sc, name, sz);
            fflush(stdout);
        }
    }

    /* Diagnose: Scene-Heap-Header (arch+0xD8) vorbelegen, damit der
       Allokator 0x284AA8 Speicher liefert (sonst bleibt base=0 und
       fn_00284D24 gibt 0 zurueck). */
    if (c != NULL && g_te.heap_seed && pc == 0x00284D80u) {
        uint32_t base = 0x0D000000u, cap = 0x02000000u, used = 0u;
        memcpy(c->mem + 0x3EC120u, &base, 4);   /* +0x20 Basis   */
        memcpy(c->mem + 0x3EC128u, &cap, 4);    /* +0x28 Kapaz.  */
        memcpy(c->mem + 0x3EC12Cu, &used, 4);   /* +0x2C belegt  */
        c->mem[0x3EC132u] = 2;                  /* +0x32 Align   */
        c->mem[0x3EC133u] = 0;                  /* +0x33 Flags   */
    }

    if (c != NULL && g_te.romfs_log &&
        (pc == 0x00289C44u || pc == 0x00289C78u || pc == 0x00289C00u)) {
        printf("[wrk2] pc=%08X r0=%08X r1=%08X r2=%08X r5=%08X obj=%08X\n",
               pc, c->r[0], c->r[1], c->r[2], c->r[5], c->r[4]);
        fflush(stdout);
    }
    if (c != NULL && g_te.romfs_log && pc == 0x00289C38u) {
        printf("[wrk] buf=%08X size=%08X align=%08X obj=%08X\n",
               c->r[5], c->r[7], c->r[6], c->r[4]);
        fflush(stdout);
    }
    if (c != NULL && g_te.romfs_log && pc == 0x00289B48u) {
        uint32_t ar = c->r[1];
        printf("[arena] pc=%08X base=%08X cap=%08X used=%08X req=%08X obj=%08X\n",
               pc, arm_mem_read(c, ar + 0x20u, 4), arm_mem_read(c, ar + 0x28u, 4),
               arm_mem_read(c, ar + 0x2Cu, 4), c->r[2], c->r[0]);
        fflush(stdout);
    }
    /* Diagnose: memcpy-Beschleunigung (fn_00291B60 ist auf `bx lr` gepatcht,
       siehe tools/patch_fast_memcpy.py). r0=Ziel, r1=Quelle, r2=Groesse. */
    if (c != NULL && g_te.fast_memcpy && (pc == 0x00291B60u || pc == 0x0027D9B4u)) {
        uint32_t dst = c->r[0], src = c->r[1], n = c->r[2];
        if (n > 0x08000000u) n = 0x08000000u;
        if ((dst & 0x1FFFFFFFu) + (uint64_t)n <= c->mem_size &&
            (src & 0x1FFFFFFFu) + (uint64_t)n <= c->mem_size) {
            memmove(c->mem + (dst & 0x1FFFFFFFu), c->mem + (src & 0x1FFFFFFFu), n);
        }
        c->r[0] = dst;   /* memcpy liefert das Ziel zurueck */
    }

    /* Diagnose: Registry-Descriptor beim Asset-Lookup zeigen. */
    if (c != NULL && g_te.romfs_log &&
        (pc == 0x00103B7Cu || pc == 0x002B95F8u)) {
        printf("[registry] pc=%08X desc: %08X %08X %08X %08X (count=%u)\n",
               pc,
               arm_mem_read(c, 0x003ECA5Cu, 4),
               arm_mem_read(c, 0x003ECA60u, 4),
               arm_mem_read(c, 0x003ECA64u, 4),
               arm_mem_read(c, 0x003ECA68u, 4),
               arm_mem_read(c, 0x003ECA64u, 4));
        fflush(stdout);
    }

    if (c != NULL && g_te.romfs_log &&
        (pc == 0x00284DD8u || pc == 0x00284EA0u || pc == 0x0012BC58u ||
         pc == 0x00284D80u)) {
        printf("[heap] pc=%08X base=%08X cap=%08X used=%08X req=%08X\n",
               pc,
               arm_mem_read(c, 0x003EC120u, 4),
               arm_mem_read(c, 0x003EC128u, 4),
               arm_mem_read(c, 0x003EC12Cu, 4),
               c->r[2]);
        fflush(stdout);
    }
    if (c != NULL && g_te.romfs_log && pc == 0x0012BC58u) {
        uint32_t sc = c->r[1];
        printf("[scene] %08X E0..108:", sc);
        for (int i = 0; i < 11; i++)
            printf(" %08X", arm_mem_read(c, sc + 0xE0u + (uint32_t)i * 4u, 4));
        printf("\n");
        fflush(stdout);
    }
    if (c != NULL && g_te.romfs_log &&
        ((pc >= 0x00284D24u && pc < 0x002851ACu) ||
         (pc >= 0x0011B98Cu && pc < 0x0011BAB4u) ||
         (pc >= 0x0012BC58u && pc < 0x0012BEBCu) ||
         (pc >= 0x00289AA0u && pc < 0x00289C00u))) {
        static int rn = 0;
        if (rn < 4000) {
            rn++;
            printf("[romfs] %08X r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r6=%08X\n",
                   pc, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[6]);
            fflush(stdout);
        }
    }

    /* Diagnose: Fortschritt der Asset-Queue (fn_002895D0, Elemente 0x28 Byte).
       0x28992C = ein Element wird gepusht, 0x289928 = Batch fertig. */
    if (c != NULL && g_te.queue_log) {
        static unsigned long q_el = 0, q_batch = 0;
        if (pc == 0x0028992Cu) {
            q_el++;
            if ((q_el % 500u) == 0) {
                printf("[queue] elems=%lu batches=%lu\n", q_el, q_batch);
                fflush(stdout);
            }
        } else if (pc == 0x00289928u) {
            q_batch++;
            printf("[queue-batch] #%lu after %lu elems (r0=%u)\n",
                   q_batch, q_el, c->r[0]);
            fflush(stdout);
        } else if (pc == 0x00289858u) {
            printf("[queue-start] elems=%lu batchsize(r8)=%u\n", q_el, c->r[8]);
            fflush(stdout);
        } else if (pc == 0x001213E8u || pc == 0x00121408u || pc == 0x00121420u ||
                   pc == 0x0012145Cu || pc == 0x00121470u || pc == 0x00121488u ||
                   pc == 0x00121498u || pc == 0x00121598u || pc == 0x001215C0u ||
                   pc == 0x001215D4u || pc == 0x001215E4u || pc == 0x001215F0u) {
            static unsigned long ln3 = 0;
            ln3++;
            if (ln3 <= 40u) {
                printf("[loadpath] pc=%08X r0=%08X r4=%08X f1d=%02X cnt=%08X n=%lu\n",
                       pc, c->r[0], c->r[4],
                       arm_mem_read(c, c->r[4] + 0x1Du, 1),
                       arm_mem_read(c, c->r[4] + 0x20u, 4), ln3);
                fflush(stdout);
            }
        } else if (pc == 0x00103BD8u || pc == 0x00103C0Cu || pc == 0x001015D0u ||
                   pc == 0x00101744u || pc == 0x001014E4u || pc == 0x001016B8u ||
                   pc == 0x001036B4u || pc == 0x00103B48u || pc == 0x00103684u ||
                   pc == 0x001035D8u || pc == 0x00103610u ||
                   pc == 0x001013D8u || pc == 0x001014D4u || pc == 0x001014ECu ||
                   pc == 0x00101508u || pc == 0x0010151Cu || pc == 0x00101540u ||
                   pc == 0x00101194u || pc == 0x00101230u || pc == 0x001013F0u ||
                   pc == 0x0010123Cu ||
                   pc == 0x0011E494u || pc == 0x00280034u || pc == 0x0012493Cu ||
                   pc == 0x0027FFF0u || pc == 0x0013C46Cu || pc == 0x00284AA8u ||
                   pc == 0x00289F30u || pc == 0x00289F20u || pc == 0x0026F130u ||
                   pc == 0x00130220u || pc == 0x0013A080u || pc == 0x002690A4u ||
                   pc == 0x0027A4BCu || pc == 0x0027A4E0u || pc == 0x0027A544u ||
                   pc == 0x0027A574u || pc == 0x0027A4E8u || pc == 0x0013A154u ||
                   pc == 0x00287DC2u || pc == 0x00287DA4u ||
                   pc == 0x002926F2u || pc == 0x002926EAu || pc == 0x00292742u ||
                   pc == 0x00292702u || pc == 0x00292746u || pc == 0x00292736u ||
                   pc == 0x00292716u || pc == 0x0029271Au || pc == 0x00292728u ||
                   pc == 0x00292720u || pc == 0x002926DCu ||
                   pc == 0x00292300u || pc == 0x00292370u || pc == 0x00292650u ||
                   pc == 0x00292678u || pc == 0x002927B8u ||
                   pc == 0x00292656u || pc == 0x0029265Cu || pc == 0x00292670u ||
                   pc == 0x00292664u || pc == 0x00292668u || pc == 0x001009E4u ||
                   pc == 0x001300C8u || pc == 0x0013C4ECu) {
            static unsigned long bn = 0;
            bn++;
            printf("[branch] pc=%08X r0=%08X r1=%08X r2=%08X lr=%08X n=%lu\n",
                   pc, c->r[0], c->r[1], c->r[2], c->r[14], bn);
            fflush(stdout);
        } else if (pc == 0x00289E48u || pc == 0x00289E34u || pc == 0x00289DB8u) {
            static unsigned long tn = 0;
            tn++;
            if (tn <= 6u || (tn % 1000u) == 0) {
                printf("[task] pc=%08X r0=%08X r4=%08X n=%lu\n",
                       pc, c->r[0], c->r[4], tn);
                fflush(stdout);
            }
        } else if (pc == 0x00212D28u || pc == 0x00212D7Cu || pc == 0x00212DB8u ||
                   pc == 0x00212DB0u || pc == 0x00212DFCu) {
            static unsigned long ln2 = 0;
            ln2++;
            if (ln2 <= 8u || (ln2 % 500u) == 0) {
                printf("[lookup] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X sb=%08X r7=%08X n=%lu\n",
                       pc, c->r[0], c->r[1], c->r[2], c->r[3], c->r[9], c->r[7], ln2);
                fflush(stdout);
            }
        } else if (pc == 0x00205858u) {
            static unsigned long hn = 0;
            hn++;
            if (hn <= 6u || (hn % 2000u) == 0) {
                uint32_t sa = c->r[0] & 0x1FFFFFFFu;
                char buf[26];
                int k;
                for (k = 0; k < 24 && sa + (uint32_t)k < c->mem_size; k++) {
                    char ch = (char)c->mem[sa + (uint32_t)k];
                    buf[k] = (ch >= 32 && ch < 127) ? ch : '.';
                }
                buf[k] = 0;
                printf("[hash] n=%lu r0=%08X r1=%08X r6=%08X '%s'\n",
                       hn, c->r[0], c->r[1], c->r[6], buf);
                fflush(stdout);
            }
        } else if (pc == 0x00283418u) {
            static unsigned long an = 0;
            an++;
            if (an <= 4u || (an % 1000u) == 0) {
                printf("[arena-free] n=%lu obj=%08X lr=%08X\n", an, c->r[0], c->r[14]);
                fflush(stdout);
            }
        } else if (pc == 0x00274CA8u) {
            static unsigned long ln = 0;
            ln++;
            if (ln <= 5u) {
                printf("[rmlist] n=%lu node=%08X lr=%08X r1=%08X\n",
                       ln, c->r[0], c->r[14], c->r[1]);
                fflush(stdout);
            } else if ((ln % 200u) == 0) {
                printf("[rmlist] n=%lu node=%08X lr=%08X\n", ln, c->r[0], c->r[14]);
                fflush(stdout);
            }
        } else if (pc == 0x001233C0u) {
            static unsigned long dn2 = 0;
            static unsigned long long t0 = 0;
            unsigned long long now2 = (unsigned long long)GetTickCount64();
            if (dn2 == 0) t0 = now2;
            dn2++;
            if (dn2 <= 6 || (dn2 % 100u) == 0) {
                printf("[dtor] n=%lu t=%llu ms r4=%u\n", dn2, now2 - t0, c->r[4]);
                fflush(stdout);
            }
        } else if (pc == 0x001233C4u) {
            static unsigned long dn = 0;
            dn++;
            if ((dn % 50u) == 0) {
                printf("[dtor] n=%lu r4=%u\n", dn, c->r[4]);
                fflush(stdout);
            }
        }
    }

    /* Diagnose: Ergebnis der Archiv-Query in der Asset-Registrierung zeigen. */
    if (c != NULL && pc == 0x00103BD8u && g_te.fa_debug) {
        uint32_t out1 = arm_mem_read(c, c->r[13] + 0x10u, 4);
        uint32_t out2 = arm_mem_read(c, c->r[13] + 0x0Cu, 4);
        printf("[assetreg] r0=%08X r7=%08X out10=%08X out0C=%08X\n",
               c->r[0], c->r[7], out1, out2);
        fflush(stdout);
    }
    /* Diagnose: der Registrierung einen gueltigen Eintrag/Offset liefern. */
    if (c != NULL && pc == 0x00103BD8u && g_te.fa_reg_ok) {
        uint32_t ent = 0x00403200u, off = 0x00000000u;
        memcpy(c->mem + ((c->r[13] + 0x10u) & 0x1FFFFFFFu), &ent, 4);
        memcpy(c->mem + ((c->r[13] + 0x0Cu) & 0x1FFFFFFFu), &off, 4);
    }

    /* Diagnose: den gesuchten Hash in den synthetischen Namespace-Eintrag
       schreiben, damit 0x24FD74 ihn garantiert findet. */
    if (c != NULL && pc == 0x0024FD74u && g_te.reg_enable &&
        c->r[1] == 0x00403000u) {
        uint32_t h = c->r[0];
        memcpy(c->mem + 0x00403100u, &h, 4);
    }

    /* Diagnose: Kind-Ebene des FA-Baums ebenfalls auf den gesuchten Hash
       biegen (Block 0x22ADE4 hat den Hash in r11). */
    if (c != NULL && pc == 0x0022ADE4u && g_te.reg_enable) {
        uint32_t h2 = c->r[11];
        memcpy(c->mem + 0x00403200u, &h2, 4);
    }

    /* Diagnose: FA-Resolver immer als "gefunden" behandeln, damit die Engine
       anschliessend die Datei ueber fs:USER oeffnen muss. */
    if (c != NULL && pc == 0x0036C5CCu && g_te.fa_fake_ok) {
        /* Nicht-leeren Namen vortaeuschen, damit der movne/bne-Zweig nach
           0x36C600 fuehrt (dort wird r0=1 erzwungen). */
        uint8_t one_byte = 1;
        if ((c->r[4] & 0x1FFFFFFFu) + 1u <= c->mem_size)
            memcpy(c->mem + (c->r[4] & 0x1FFFFFFFu), &one_byte, 1);
    }
    if (c != NULL && pc == 0x0036C600u && g_te.fa_fake_ok)
        c->r[0] = 1;

    /* Diagnose: Hash-Vergleich im FA-Verzeichnis immer "gleich" (die Suche
       findet damit jeden Namen im ersten Eintrag). */
    if (c != NULL && pc == 0x00390C98u && g_te.fa_always)
        c->r[1] = c->r[0];

    /* Diagnose: Blockpfad in [lo, lo+0x4000) protokollieren. */
    if (c != NULL && g_te.blklogf) {
        static uint32_t last = 0xFFFFFFFFu;
        uint32_t lo = g_te.blklog_lo;
        if (pc >= lo && pc < lo + 0x4000u) {
            if (pc != last) {
                last = pc;
                printf("[blk] %08X r0=%08X r4=%08X r8=%08X\n",
                       pc, c->r[0], c->r[4], c->r[8]);
                fflush(stdout);
            }
        }
    }

    /* 0x101364 bl 0x1034F4 -> bei 0x101370 wird sonst die gesamte
       Asset-Registrierung (0x1013F0) uebersprungen. */
    if (c != NULL && g_te.force_asset &&
        (pc == 0x00101368u || pc == 0x00101370u || pc == 0x00101374u)) {
        static int fc2 = 0;
        if (fc2 < 5) { fc2++; printf("[forceasset] pc=%08X r0=%08X\n", pc, c->r[0]); }
        c->r[0] = 1;
    }
    /* Diagnose: an den Datei-Open-Fatal-Sites die Register zeigen. */
    if (c != NULL && g_te.dump_fatal &&
        (pc == 0x00107A4Cu || pc == 0x00107B34u)) {
        printf("[fatal-site] pc=%08X r0=%08X r1=%08X r2=%08X r3=%08X "
               "r4=%08X r5=%08X r6=%08X r7=%08X\n",
               pc, c->r[0], c->r[1], c->r[2], c->r[3],
               c->r[4], c->r[5], c->r[6], c->r[7]);
        fflush(stdout);
    }

    /* Diagnose: beim "rom:"-Open-Wrapper die Struktur hinter r3 zeigen. */
    if (c != NULL && pc == 0x00107AE4u && g_te.dump_r3) {
        uint32_t a = c->r[3] & 0x1FFFFFFFu;
        printf("[r3] r1=%08X r2=%08X r3=%08X:", c->r[1], c->r[2], c->r[3]);
        for (int i = 0; i < 8 && a + i * 4 + 4 <= c->mem_size; i++) {
            uint32_t w = 0;
            memcpy(&w, c->mem + a + i * 4, 4);
            printf(" %08X", w);
        }
        printf("\n");
        fflush(stdout);
    }
    if (arm_pump_hook != NULL) arm_pump_hook();
    if (arm_tick_hook != NULL) arm_tick_hook(c);
    if (c != NULL) {
        {
            uint32_t ti = (c->tpidruro >> 12) & 0xFu;
            arm_thread_last_pc[ti] = pc;
            arm_thread_last_lr[ti] = c->r[14];
            if (ti < 16) { g_t_ring[ti][g_t_pos[ti] & 63u] = pc; g_t_ring_lr[ti][g_t_pos[ti] & 63u] = c->r[14]; g_t_pos[ti]++; }
        }
        g_pc_ring[g_pc_pos] = pc;
        g_lr_ring[g_pc_pos] = c->r[14];
        g_pc_pos = (g_pc_pos + 1) & (PC_RING - 1);
    }
    if (arm_watch_pc != 0 && pc == arm_watch_pc && c != NULL && g_watch_hits < 600) {
        g_watch_hits++;
        fprintf(stderr, "[watch] 0x%08X r0=%08X r1=%08X r2=%08X r3=%08X "
                        "r4=%08X r5=%08X r6=%08X r7=%08X sp=%08X lr=%08X\n",
                pc, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
                c->r[7], c->r[13], c->r[14]);
        {
            uint32_t s8 = (c->r[13] + 8) & ~3u;
            if (s8 + 8 <= c->mem_size) {
                fprintf(stderr, "[watch]   [sp+8]=%08X [sp+0x20]=%08X\n",
                        (unsigned)arm_mem_read(c, s8, 4),
                        (unsigned)arm_mem_read(c, (c->r[13] + 0x20) & ~3u, 4));
            }
        }
        {
            uint32_t a = c->r[4] & ~3u;
            if (a + 16 <= c->mem_size) {
                fprintf(stderr, "[watch]   mem@r4: %08X %08X %08X %08X\n",
                        (unsigned)arm_mem_read(c, a, 4), (unsigned)arm_mem_read(c, a + 4, 4),
                        (unsigned)arm_mem_read(c, a + 8, 4), (unsigned)arm_mem_read(c, a + 12, 4));
            }
        }
        if (g_te.watch_pool) {
            uint32_t a = 0x004080C8u;
            fprintf(stderr, "[watch]   pool:");
            for (int i = 0; i < 16; i++)
                fprintf(stderr, " %08X", (unsigned)arm_mem_read(c, a + (uint32_t)i * 4u, 4));
            fprintf(stderr, "\n");
        }
        fflush(stderr);
    }
    if (!g_file) return;
    if (g_records >= g_max_records) return;
    if (g_unique_only && g_seen != NULL && !mark_seen(pc)) return;
    g_records++;
    if ((++g_flush_counter & 0x3Fu) == 0) {
        fflush(g_file);
    }
    put_u8('B');
    put_u32(pc);
    put_u8(thumb ? 1 : 0);
}

void arm_trace_svc(uint32_t imm) {
    if (!g_file) return;
    put_u8('S');
    put_u32(imm);
}

void arm_trace_ipc(const char *service, uint32_t cmd_id) {
    if (!g_file) return;
    const char *name = service ? service : "";
    size_t len = strlen(name);
    if (len > 64) len = 64;
    put_u8('I');
    put_u8((unsigned)len);
    if (fwrite(name, 1, len, g_file) != len) { /* ignore */ }
    put_u16((unsigned)(cmd_id & 0xFFFF));
}

void arm_trace_frame(void) {
    static unsigned counter = 0;
    if (!g_file) return;
    put_u8('F');
    put_u32(counter++);
}

void arm_trace_event(uint32_t kind, uint32_t a, uint32_t b) {
    if (!g_file) return;
    put_u8('E');
    put_u32(kind);
    put_u32(a);
    put_u32(b);
}
