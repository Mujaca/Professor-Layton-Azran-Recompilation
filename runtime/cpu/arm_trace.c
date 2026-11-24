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
/* Letzter Registerstand je Gast-Thread (nur Diagnose HLE_STALLDUMP). */
uint32_t arm_thread_last_regs[16][16];
static ArmCpu *g_t_cpu[16];
static int g_t_regs_on = -1;

void arm_trace_thread_regs(int ti) {
    if (ti < 0 || ti > 15) return;
    ArmCpu *c = g_t_cpu[ti];
    uint32_t *r = arm_thread_last_regs[ti];
    if (c == NULL) return;
    fprintf(stderr, "[tdump]   regs r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X "
                    "r5=%08X r6=%08X r7=%08X\n", r[0], r[1], r[2], r[3], r[4],
            r[5], r[6], r[7]);
    fprintf(stderr, "[tdump]   regs r8=%08X r9=%08X r10=%08X r11=%08X "
                    "r12=%08X sp=%08X lr=%08X\n", r[8], r[9], r[10], r[11],
            r[12], r[13], r[14]);
    /* Speicher an den haeufig interessanten Basisregistern (r0/r1/r12). */
    uint32_t obj = r[0];
    uint32_t tbl = 0, tbl2 = 0;
    if ((uint64_t)obj + 0x3BCu <= c->mem_size) {
        memcpy(&tbl, c->mem + obj + 0x3B8u, 4);
        if ((uint64_t)tbl + 4u <= c->mem_size) memcpy(&tbl2, c->mem + tbl, 4);
    }
    struct { const char *name; uint32_t addr; } spots[11] = {
        {"r0", r[0]}, {"r0+78", r[0] + 0x78u}, {"r0+1e0", r[0] + 0x1E0u},
        {"r0+3b8", r[0] + 0x3B8u}, {"r1", r[1]},
        {"r1-40", r[1] - 0x40u}, {"r1-200", r[1] - 0x200u}, {"r12", r[12]},
        {"tbl", tbl}, {"r0+38", r[0] + 0x38u}, {"r0+40", r[0] + 0x40u}};
    fprintf(stderr, "[tdump]   ptbl=%08X [tbl]=%08X\n", tbl, tbl2);
    for (int k = 0; k < 11; k++) {
      uint32_t a = spots[k].addr;
      if ((uint64_t)a + 32u <= c->mem_size) {
        fprintf(stderr, "[tdump]   mem %s:", spots[k].name);
        for (int i = 0; i < 8; i++) {
            uint32_t v;
            memcpy(&v, c->mem + a + (uint32_t)i * 4u, 4);
            fprintf(stderr, " %08X", v);
        }
        fprintf(stderr, "\n");
      }
    }
    /* Stackwoerter: Rueckkehrkette der aufrufenden Funktionen. */
    uint32_t sp = r[13];
    if ((uint64_t)sp + 160u <= c->mem_size) {
        fprintf(stderr, "[tdump]   stack:");
        for (int i = 0; i < 40; i++) {
            uint32_t v;
            memcpy(&v, c->mem + sp + (uint32_t)i * 4u, 4);
            fprintf(stderr, " %08X", v);
        }
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

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
    if (!init) {
        init = 1;
        if (getenv("HLE_PERFMAP") != NULL || getenv("HLE_PERFMAP_FILE") != NULL)
            perfmap_init();
    }
    if (g_pm_on && c != NULL) perfmap_sample(c, pc);
    if (arm_pump_hook != NULL) arm_pump_hook();
    if (c != NULL) {
        uint32_t ti = (c->tpidruro >> 12) & 0xFu;
        arm_thread_last_pc[ti] = pc;
        arm_thread_last_lr[ti] = c->r[14];
        /* Registerstand nur fuer HLE_STALLDUMP mitschreiben (Hot Path). */
        if (g_t_regs_on < 0)
            g_t_regs_on = (getenv("HLE_STALLDUMP") != NULL) ? 1 : 0;
        if (g_t_regs_on) {
            g_t_cpu[ti] = c;
            memcpy(arm_thread_last_regs[ti], c->r,
                   sizeof(arm_thread_last_regs[ti]));
        }
        g_t_ring[ti][g_t_pos[ti] & 63u] = pc;
        g_t_ring_lr[ti][g_t_pos[ti] & 63u] = c->r[14];
        g_t_pos[ti]++;
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

