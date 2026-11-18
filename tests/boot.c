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
            if (getenv("HLE_DUMPIDLE")) {
                static const unsigned addrs[] = { 0x40B370u, 0x40B4E8u, 0x40B4E0u,
                                                  0x40B4ECu, 0x40B378u, 0x40B374u,
                                                  0x40B36Cu, 0x40B37Cu, 0x3D91E8u,
                                                  0x3D91ECu, 0x3D91F8u, 0x3D9200u,
                                                  0x3D92ECu, 0x3D91FCu };
                for (unsigned a = 0; a < sizeof(addrs) / sizeof(addrs[0]); a++) {
                    uint32_t v = 0;
                    memcpy(&v, g_mem + (addrs[a] & 0x1FFFFFFFu), 4);
                    printf("[idle] [%08X] = %08X\n", addrs[a], v);
                }
            }
            for (int ti = 0; ti < 16; ti++) {
                if (arm_thread_last_pc[ti])
                    printf("[wd]   thread %X: pc=0x%08X lr=0x%08X\n", ti,
                           arm_thread_last_pc[ti], arm_thread_last_lr[ti]);
            }
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

    printf("boot: entry 0x00100000, romfs=%s\n", romfs);

    fn_00100000(&c);

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
