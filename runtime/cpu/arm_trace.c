/* Block-Hook und Diagnose fuer den recompilierten Code.
 *
 * Der Recompiler ruft arm_trace_block() am Anfang jedes Basisblocks.
 * Hier passiert nur:
 *   - arm_last_block / Thread-Ringpuffer (Diagnose bei Abbruch),
 *   - der Scheduler-Hook (kooperatives Threading, runtime/kernel),
 *   - optionale, rein lesende Diagnose: ARM_WATCH, Perfmap, CTRTRC-Trace.
 * Hier darf KEIN spielspezifischer Code stehen (siehe AGENTS.md).
 */
#include "arm_trace.h"
#include "arm_cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

uint32_t arm_last_block = 0;
void (*arm_pump_hook)(void) = NULL;
uint32_t arm_watch_pc = 0;
static int g_watch_hits = 0;

/* ------------------------------------------------------------------ *
 * ARM_DUMP=0xADDR:pfad (reine Diagnose, nur lesend)
 *
 * Beim ersten Erreichen des Basisblocks ADDR wird ein vollstaendiger
 * Gastzustand geschrieben: Register/Flags und alle nichtleeren 4-KiB-
 * Seiten des Gast-Speichers (Codebereich ausgenommen, den liest das
 * Werkzeug aus out/exefs/code_decomp.bin). Damit kann dieselbe Funktion
 * einmal im Port und einmal in Unicorn mit den Originalbytes laufen.
 * Das Format liest tools/func_difftest.py.
 * ------------------------------------------------------------------ */
static uint32_t g_dump_pc = 0;
static char g_dump_path[600];
static int g_dump_armed = 0;
static int g_dump_done = 0;

/* ARM_DUMP_LIST=0xADDR,0xADDR,... zusammen mit ARM_DUMP_DIR=<verz>:
 * je Adresse hoechstens ein Schnappschuss <verz>/dump_XXXXXXXX.bin. Damit
 * kann ein einziger Bootlauf viele Funktionseinstiege belegen, ohne dass
 * pro Einstieg ein eigener Lauf noetig ist (tools/cosim/sweep.py). Reine
 * Diagnose, nur lesend. */
#define DUMP_LIST_MAX 512
static uint32_t g_dump_list[DUMP_LIST_MAX];
static unsigned char g_dump_list_hit[DUMP_LIST_MAX];
static unsigned g_dump_list_n = 0;
static char g_dump_dir[512];

static void dump_arm(void)
{
    const char *p = getenv("ARM_DUMP");
    const char *list, *dir;
    char *colon;
    if (p != NULL && *p != '\0') {
        g_dump_armed = 1;
        g_dump_pc = (uint32_t)strtoul(p, &colon, 0);
        while (*colon == ' ' || *colon == '\t' || *colon == ':' || *colon == '=')
            colon++;
        snprintf(g_dump_path, sizeof g_dump_path, "%s", colon);
    }
    list = getenv("ARM_DUMP_LIST");
    dir = getenv("ARM_DUMP_DIR");
    if (list == NULL || *list == '\0' || dir == NULL || *dir == '\0') return;
    snprintf(g_dump_dir, sizeof g_dump_dir, "%s", dir);
    while (*list != '\0' && g_dump_list_n < DUMP_LIST_MAX) {
        g_dump_list[g_dump_list_n++] = (uint32_t)strtoul(list, &colon, 0);
        if (colon == list) break;
        list = colon;
        while (*list == ',' || *list == ' ' || *list == '\t') list++;
    }
}

static void dump_put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8),
                           (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

static int page_nonzero(const unsigned char *p, uint32_t size)
{
    uint32_t i = 0;
    uint64_t acc = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        acc |= w;
    }
    for (; i < size; i++) acc |= p[i];
    return acc != 0;
}

static void dump_state_to(ArmCpu *c, uint32_t pc, const char *path)
{
    FILE *f = fopen(path, "wb");
    uint32_t regions[4096][2];
    unsigned nregions = 0;
    uint32_t addr;
    if (f == NULL) {
        fprintf(stderr, "[dump] kann %s nicht schreiben\n", path);
        return;
    }
    fwrite("ARMDMP01", 1, 8, f);
    dump_put32(f, arm_last_block);
    for (int i = 0; i < 16; i++) dump_put32(f, c->r[i]);
    dump_put32(f, c->n);
    dump_put32(f, c->z);
    dump_put32(f, c->c);
    dump_put32(f, c->v);
    dump_put32(f, c->qflag);
    dump_put32(f, c->ge);
    dump_put32(f, c->t);
    dump_put32(f, c->cpsr_mode);
    dump_put32(f, c->tpidruro);
    dump_put32(f, c->fpscr);
    dump_put32(f, 0);                 /* Platzhalter: Anzahl Regionen */
    /* Nichtleere Seiten sammeln. Der Codebereich wird ausgelassen. */
    for (addr = 0; addr + 4096u <= c->mem_size && addr < 0x20000000u;
         addr += 4096u) {
        if (addr >= 0x00100000u && addr < 0x00400000u) continue;
        if (!page_nonzero(c->mem + addr, 4096u)) continue;
        if (nregions > 0 &&
            regions[nregions - 1][0] + regions[nregions - 1][1] == addr) {
            regions[nregions - 1][1] += 4096u;
        } else if (nregions < 4096u) {
            regions[nregions][0] = addr;
            regions[nregions][1] = 4096u;
            nregions++;
        }
    }
    for (unsigned i = 0; i < nregions; i++) {
        dump_put32(f, regions[i][0]);
        dump_put32(f, regions[i][1]);
        fwrite(c->mem + regions[i][0], 1, regions[i][1], f);
    }
    fseek(f, 8 + 4 + 16 * 4 + 10 * 4, SEEK_SET);
    dump_put32(f, nregions);
    fclose(f);
    printf("[dump] 0x%08X: %u Regionen -> %s\n", pc, nregions, path);
    fflush(stdout);
}

static void dump_state(ArmCpu *c)
{
    dump_state_to(c, g_dump_pc, g_dump_path);
}

/* ------------------------------------------------------------------ *
 * HLE_PERF=1: Blockrate des generierten Codes (reine Diagnose).
 * Zaehlt jeden ausgefuehrten Basisblock und meldet beim Prozessende
 * "[perf] N blocks in M ms (R/s)" (Format von tools/g3ds/runner.py).
 * Ohne HLE_PERF bleibt nur der Branch auf g_perf_on.
 * ------------------------------------------------------------------ */
static int g_perf_on = 0;
static int g_perf_armed = 0;
static uint64_t g_perf_blocks = 0;
static uint64_t g_perf_t0 = 0;

static uint64_t perf_now_ns(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static void perf_report(void) {
    uint64_t dt = perf_now_ns() - g_perf_t0;
    uint64_t ms = dt / 1000000u;
    double rate = dt ? ((double)g_perf_blocks * 1e9 / (double)dt) : 0.0;
    printf("[perf] %llu blocks in %llu ms (%.0f/s)\n",
           (unsigned long long)g_perf_blocks, (unsigned long long)ms, rate);
    fflush(stdout);
}

static void perf_arm(void) {
    if (g_perf_on || g_perf_armed) return;
    g_perf_armed = 1;
    if (getenv("HLE_PERF") == NULL) return;
    g_perf_on = 1;
    g_perf_t0 = perf_now_ns();
    atexit(perf_report);
}

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
void arm_perfmap_report(void);
static void perfmap_sample(ArmCpu *c, uint32_t pc);
static void perfmap_init(void);


/* Ringpuffer der zuletzt ausgefuehrten Bloecke (Diagnose bei Abbruch).
 * Wird nur aufgezeichnet, wenn eine Diagnose sie braucht: die vier Stores
 * plus Zaehler je Block kosten im Hot Path messbar Zeit. */
static int g_ring_on = 0;

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

void arm_trace_block(ArmCpu *c, uint32_t pc, uint32_t thumb) {
    static int init = 0;
    arm_last_block = pc;
    if (g_perf_on) g_perf_blocks++;
    if (!init) {
        init = 1;
        perf_arm();
        dump_arm();
        if (getenv("HLE_PERFMAP") != NULL || getenv("HLE_PERFMAP_FILE") != NULL)
            perfmap_init();
        /* Ring nur, wenn eine Diagnose ihn auswertet (HLE_STALLDUMP,
           ARM_WATCH, Perfmap, ARM_RING). Default aus = schneller Hot Path. */
        if (getenv("ARM_RING") != NULL || getenv("HLE_STALLDUMP") != NULL ||
            getenv("ARM_WATCH") != NULL || getenv("HLE_PERFMAP") != NULL ||
            getenv("HLE_PERFMAP_FILE") != NULL)
            g_ring_on = 1;
    }
    if (g_pm_on && c != NULL) perfmap_sample(c, pc);
    if (arm_pump_hook != NULL) arm_pump_hook();
    if (g_dump_armed && !g_dump_done && c != NULL && pc == g_dump_pc) {
        g_dump_done = 1;
        dump_state(c);
    }
    if (g_dump_list_n != 0 && c != NULL) {
        for (unsigned i = 0; i < g_dump_list_n; i++) {
            if (g_dump_list_hit[i] || g_dump_list[i] != pc) continue;
            char path[700];
            g_dump_list_hit[i] = 1;
            snprintf(path, sizeof path, "%s/dump_%08X.bin", g_dump_dir, pc);
            dump_state_to(c, pc, path);
            break;
        }
    }
    if (c != NULL) {
        if (g_ring_on) {
            uint32_t ti = (c->tpidruro >> 12) & 0xFu;
            arm_thread_last_pc[ti] = pc;
            arm_thread_last_lr[ti] = c->r[14];
            g_t_ring[ti][g_t_pos[ti] & 63u] = pc;
            g_t_ring_lr[ti][g_t_pos[ti] & 63u] = c->r[14];
            g_t_pos[ti]++;
            g_pc_ring[g_pc_pos] = pc;
            g_lr_ring[g_pc_pos] = c->r[14];
            g_pc_pos = (g_pc_pos + 1) & (PC_RING - 1);
        } else {
            /* Nur der letzte Block je Thread (fuer Statuszeile/Fehlermeldungen). */
            uint32_t ti = (c->tpidruro >> 12) & 0xFu;
            arm_thread_last_pc[ti] = pc;
            arm_thread_last_lr[ti] = c->r[14];
        }
    }
    if (arm_watch_pc != 0 && pc == arm_watch_pc && c != NULL && g_watch_hits < 600) {
        g_watch_hits++;
        fprintf(stderr, "[watch] 0x%08X r0=%08X r1=%08X r2=%08X r3=%08X "
                        "r4=%08X r5=%08X r6=%08X r7=%08X sp=%08X lr=%08X\n",
                pc, c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5], c->r[6],
                c->r[7], c->r[13], c->r[14]);
        fflush(stderr);
    }
    if (!g_file) return;
    if (g_records >= g_max_records) return;
    if (g_unique_only && g_seen != NULL && !mark_seen(pc)) return;
    g_records++;
    if ((++g_flush_counter & 0x3Fu) == 0) fflush(g_file);
    put_u8('B');
    put_u32(pc);
    put_u8(thumb ? 1 : 0);
}

/* Vom Recompiler fuer ldr-pc-Sprungtabellen emittiert (Diagnose aus). */
int arm_ldrpc_log_on(void) { return 0; }
void arm_ldrpc_log(unsigned t) { (void)t; }

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

