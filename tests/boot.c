/* Boot-Host: fuehrt den recompilierten Code mit flachem 3DS-Speicher aus.
 * Phase A: alles loggen, Services noch generisch beantworten. */
#include "arm_cpu.h"
#include "arm_trace.h"
#include "hle.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "hle.h"
#include "gpu.h"

#define MEM_SIZE 0x30000000u   /* 768 MiB: FCRAM + VRAM (bis 0x20500000) + MMIO */

extern uint32_t arm_thread_last_pc[16];
extern void arm_trace_thread_dump(int ti, unsigned n);
extern uint32_t arm_thread_last_lr[16];

void fn_00100000(ArmCpu *c);

static unsigned char *g_mem;
static volatile ArmCpu *g_cpu_view = NULL;

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    for (;;) {
        Sleep(2000);
        static int ticks = 0;
        if (g_cpu_view) {
            ticks++;
            printf("[wd] pc=0x%08X sp=0x%08X lr=0x%08X\n",
                   g_cpu_view->r[15], g_cpu_view->r[13], g_cpu_view->r[14]);
            { int tc = 0; unsigned long long tk = 0; hle_tick_stats(&tc, &tk);
              printf("[wd] tickcalls=%d tick=%llu\n", tc, tk); }
            if (getenv("HLE_DUMP_RANGE")) {
                if (1) {
                    const char *spec = getenv("HLE_DUMP_RANGE");
                    unsigned lo = 0, hi = 0;
                    if (sscanf(spec, "%x,%x", &lo, &hi) == 2 && hi > lo) {
                        for (unsigned a = lo; a + 4 <= hi; a += 4) {
                            uint32_t w = 0;
                            memcpy(&w, g_mem + (a & 0x1FFFFFFFu), 4);
                            printf("[range] %08X = %08X\n", a, w);
                        }
                    }
                }
            }
            if (getenv("HLE_DUMP_REG")) {
                static int found = 0;
                if (!found) {
                    static const char needle[] = "arc/lt6_a.fa";
                    const size_t nl = sizeof(needle) - 1;
                    static const uint32_t ranges[6] = { 0x3E0000u, 0x420000u,
                                                        0x08000000u, 0x10000000u,
                                                        0x14000000u, 0x1C000000u };
                    for (int r = 0; r < 6; r += 2) {
                        for (uint32_t a = ranges[r]; a + nl < ranges[r + 1]; a++) {
                            if (memcmp(g_mem + (a & 0x1FFFFFFFu), needle, nl) != 0)
                                continue;
                            printf("[reg-found] %08X:", a);
                            for (int k = 0; k < 8; k++) {
                                uint32_t w = 0;
                                uint32_t o = a - 32 + k * 4;
                                memcpy(&w, g_mem + (o & 0x1FFFFFFFu), 4);
                                printf(" %08X", w);
                            }
                            printf("\n");
                            found = 1;
                            break;
                        }
                        if (found) break;
                    }
                }
            }
            if (getenv("HLE_DUMPIDLE")) {
                static const unsigned addrs[] = { 0x40B370u, 0x40B4E8u, 0x40B4E0u,
                                                  0x40B4ECu, 0x40B378u, 0x40B374u,
                                                  0x40B36Cu, 0x40B37Cu, 0x3D91E8u,
                                                  0x3D91ECu, 0x3D91F8u, 0x3D9200u,
                                                  0x3D92ECu, 0x3D91FCu,
                                                  0x3ECA5Cu, 0x3ECA60u, 0x3ECA64u,
                                                  0x3EC028u, 0x3EC02Cu };
                for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
                    uint32_t v = 0;
                    memcpy(&v, g_mem + (addrs[a] & 0x1FFFFFFFu), 4);
                    printf("[idle] [%08X] = %08X\n", addrs[a], v);
                    /* Verkettete Asset-Registry verfolgen (max. 3 Eintraege). */
                    if (addrs[a] == 0x3ECA5Cu && v >= 0x08000000u && v < 0x20000000u) {
                        uint32_t cur = v;
                        for (int d = 0; d < 3 && cur >= 0x08000000u && cur < 0x20000000u; d++) {
                            uint32_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;
                            memcpy(&w0, g_mem + (cur & 0x1FFFFFFFu), 4);
                            memcpy(&w1, g_mem + ((cur + 4) & 0x1FFFFFFFu), 4);
                            memcpy(&w2, g_mem + ((cur + 8) & 0x1FFFFFFFu), 4);
                            memcpy(&w3, g_mem + ((cur + 12) & 0x1FFFFFFFu), 4);
                            printf("[reg] %08X: %08X %08X %08X %08X\n", cur, w0, w1, w2, w3);
                            if (w2 < 0x08000000u || w2 >= 0x20000000u) break;
                            cur = w2;
                        }
                    }
                }
            }
            for (int ti = 0; ti < 16; ti++) {
                if (arm_thread_last_pc[ti])
                    printf("[wd]   thread %X: pc=0x%08X lr=0x%08X\n", ti,
                           arm_thread_last_pc[ti], arm_thread_last_lr[ti]);
            }
            fflush(stdout);
            if (ticks == 4 || ticks == 14 || ticks == 29) {
                fprintf(stderr, "[wd] ==== tdump bei tick %d ====\n", ticks);
                arm_trace_thread_dump(2, 64);
                for (int ti = 3; ti < 16; ti++)
                    if (arm_thread_last_pc[ti]) arm_trace_thread_dump(ti, 8);
                fflush(stderr);
            }
            if (g_cpu_view->aborted) {
                hle_report();
                exit(3);
            }
        }
    }
    return 0;
}

static void load_code(ArmCpu *c) {
    FILE *f = fopen("out/exefs/code_decomp.bin", "rb");
    if (!f) {
        printf("code_decomp.bin fehlt\n");
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0x300000) n = 0x300000;
    if (fread(g_mem + 0x100000, 1, (size_t)n, f) != (size_t)n) {
        printf("code kurz gelesen\n");
    }
    fclose(f);
    /* Gezielte Code-Patches (Diagnose): SDK-Fehlerpfade entschaerfen.
     * 0x212CC0: "bleq 0x28F45C" (wirft FS-Fehler, wenn ein Session-Handle 0 ist)
     *           -> NOP, damit der Aufruf ohne Abbruch weiterlaeuft. */
    if (0x212CC0u + 4 <= 0x300000u) {
        *(uint32_t *)(g_mem + 0x212CC0) = 0xE1A00000u;
        *(uint32_t *)(g_mem + 0x212FCC) = 0xE1A00000u;
        /* Diagnose: RomFS-Mount (fn_00284D24) nicht frueh aussteigen lassen
           (beq 0x285188 nach fn_0011B98C) -> der Guard [archive+0xA34] wird
           gesetzt und der Archiv-Resolver 0x36C54C findet Dateien. */
        if (getenv("HLE_ROMFS_FORCE"))
            *(uint32_t *)(g_mem + 0x284D68) = 0xE1A00000u;
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
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *romfs = (argc > 1) ? argv[1] : "out/romfs";
    g_mem = (unsigned char *)calloc(1, MEM_SIZE);
    if (!g_mem) { printf("kein Speicher\n"); return 2; }

    ArmCpu c;
    arm_cpu_init(&c, g_mem, MEM_SIZE);
    load_code(&c);

    c.r[13] = 0x0FFF0000u;                 /* App-Heap-Ende - Stack */
    c.r[14] = 0u;
    c.tpidruro = 0x1FF82000u;              /* TLS_AREA_VADDR */
    c.r[15] = 0x00100000u;

    if (getenv("ARM_TRACE_FILE")) {
        arm_trace_open(getenv("ARM_TRACE_FILE"));
    }

    g_cpu_view = &c;
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    hle_init(&c, romfs);
    arm_set_svc_handler(hle_svc);

    /* GPU-Selbsttest: synthetische PICA-Kommandoliste ausfuehren und
       pruefen, dass Pixel im Framebuffer landen. */
    if (getenv("HLE_GPU_TEST")) {
        uint32_t cl = 0x08001000u, n = 0;
        uint32_t list[7];
        list[0] = 0x0400FF00u;      /* ClearColor = 0x00FF00 (gruen) */
        list[1] = 0x050FFFFFu;       /* FillRect, Anzahl folgt */
        list[2] = 4u;
        list[3] = 0u;  list[4] = 0u; list[5] = 400u; list[6] = 240u;
        memcpy(g_mem + (cl & 0x1FFFFFFFu), list, sizeof(list));
        n = (uint32_t)gpu_run_command_list(&c, cl, (uint32_t)sizeof(list));
        printf("[gputest] ops=%u\n", n);
        printf("[gputest] fb[0]=%02X%02X%02X\n",
               g_mem[0x1F380000u + 2], g_mem[0x1F380000u + 1],
               g_mem[0x1F380000u + 0]);
        printf("[gputest] fb[last]=%02X%02X%02X\n",
               g_mem[0x1F380000u + 3u * (400u * 240u - 1u) + 2],
               g_mem[0x1F380000u + 3u * (400u * 240u - 1u) + 1],
               g_mem[0x1F380000u + 3u * (400u * 240u - 1u) + 0]);
    }

    printf("boot: entry 0x00100000, romfs=%s\n", romfs);

    fn_00100000(&c);

    /* Diagnose: Haupt-Entry ist zurueckgekehrt, die Engine-Threads laufen
       aber weiter (der echte 3DS-Prozess lebt, bis exit() kommt). Mit
       HLE_KEEP_ALIVE=<sek> lassen wir sie weiterlaufen. */
    {
        const char *ka = getenv("HLE_KEEP_ALIVE");
        if (ka != NULL && *ka != 0) {
            int secs = atoi(ka);
            printf("keepalive: main zurueck, warte %d s auf Threads\n", secs);
            fflush(stdout);
            for (int i = 0; i < secs; i++) {
                Sleep(1000);
                printf("keepalive %d s: t2=%08X t5=%08X t9=%08X tA=%08X\n",
                       i + 1, arm_thread_last_pc[2], arm_thread_last_pc[5],
                       arm_thread_last_pc[9], arm_thread_last_pc[10]);
                fflush(stdout);            }
        }
    }

    printf("halt: aborted=%d pc=0x%08X r0=0x%08X\n", c.aborted, c.r[15], c.r[0]);
    if (c.aborted) {
        unsigned *lo = (unsigned *)g_mem;
        printf("lowmem:");
        for (int i = 0; i < 16; i++) printf(" [%02X]=%08X", i * 4, lo[i]);
        printf("\n");
        printf("obj1C:");
        for (int i = 0; i < 12; i++) printf(" %08X", lo[7 + i]);
        printf("\n");
    }
    if (c.aborted) arm_trace_pc_ring_dump(250);
    hle_report();
    arm_trace_close();
    return 0;
}
