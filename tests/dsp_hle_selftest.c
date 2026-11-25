/* Selbsttest fuer runtime/audio/dsp_hle.c (DSP-HLE-Frame-Verarbeitung).
 *
 * Erwartungen stammen aus Azahar audio_core/hle/{hle,shared_memory,source}.cpp:
 *  - ReadRegion = Region mit hoeherem Frame-Counter (+0x7FFE jeder Region),
 *    WriteRegion = die andere (Wraparound 0xFFFF gegen 0xFFFE).
 *  - SourceConfiguration.dirty wird nach dem Verarbeiten geloescht; Status
 *    geht in die Schreibregion (is_enabled, current_buffer_id_dirty,
 *    sync_count, buffer_position).
 *  - u32_dsp ist mittelendian: Wert 160 = 0x000000A0 liegt als
 *    00 00 A0 00 im Speicher (shared_memory.h u32_dsp).
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <windows.h>

#include "dsp_hle.h"

#define REGION0 (0x1FF50000u)
#define REGION1 (0x1FF70000u)
#define SM_SOURCE_STATUS 0x0D00u
#define SM_SOURCE_CONFIG 0x3D24u
#define SM_FINAL_SAMPLES 0x0A80u
#define SM_FRAME_COUNTER 0x7FFEu
#define CFG_DIRTY 0u
#define CFG_ENABLE 160u
#define CFG_SYNC_COUNT 162u
#define CFG_EMB_PHYS 172u
#define CFG_EMB_LEN 176u
#define CFG_EMB_FLAGS1 180u
#define CFG_EMB_FLAGS2 188u
#define ST_ENABLED 0u
#define ST_DIRTY 1u
#define ST_SYNC 2u
#define ST_POS 4u

static unsigned char *g_mem;
static int g_fail;

static void check(int cond, const char *what) {
    if (!cond) {
        g_fail++;
        printf("FAIL %s\n", what);
    }
}

static void wr16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}
static void wr32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
/* u32_dsp mittelendian (shared_memory.h). */
static void wr_dsp32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 16);
    p[1] = (unsigned char)(v >> 24);
    p[2] = (unsigned char)v;
    p[3] = (unsigned char)(v >> 8);
}

static int g_int_zero, g_int_pipe;

static void irq(int type, int pipe) {
    if (type == AUDIO_DSP_INT_ZERO) g_int_zero++;
    if (type == AUDIO_DSP_INT_PIPE && pipe == 2) g_int_pipe++;
}

static unsigned char *region(int i) {
    return g_mem + (i == 0 ? REGION0 : REGION1);
}

static void set_counter(int i, unsigned fc) {
    wr16(region(i) + SM_FRAME_COUNTER, fc);
}

/* --- Takt-Modell: Frames nur zwischen Gast-Bloecken ----------------------
 * Haenger 2 (2026-11-25, Befund Commit a949191): Der DSP-Thread verarbeitete
 * Frames parallel zum Gast. Der Sound-Thread des Spiels liest den Status
 * aller Quellen nacheinander; lief dazwischen ein Frame, kam Quelle 0 aus
 * Frame N und Quelle 1 aus N+1 (current_buffer_id_dirty steht nur einen
 * Frame), ein Kanal verpasste den Pufferwechsel, das Spiel haengte einen noch
 * eingereihten Wellenpuffer erneut an (Selbstverweis, Endlosschleife).
 * Azahar tickt den DSP in core_timing zwischen Gast-Instruktionen. Im Port
 * meldet der Takt-Thread nur "Frame faellig"; ausgefuehrt wird der Frame vom
 * Gast-Scheduler (audio_dsp_run_due). Solange niemand run_due aufruft, darf
 * sich also kein Frame und kein Byte der Regionen aendern. Danach koppelt
 * der Folgeframe an die Spielantwort (neuer Frame-Counter). */
static volatile LONG g_notified;
static void dsp_notify_test(void) { InterlockedIncrement(&g_notified); }

static void thread_frames_only_via_scheduler(void) {
    static unsigned char snap[2][0x8000];
    unsigned char buf[4] = {0, 0, 0, 0};
    unsigned long long f0, f1;
    unsigned char *cfg;
    int ran;

    audio_dsp_set_notify(dsp_notify_test);
    audio_dsp_set_host_output(0);
    audio_dsp_pipe_write(buf, 4);     /* Initialize */
    set_counter(0, 30);
    set_counter(1, 29);

    /* Zwei loopende PCM16-Quellen (Kanal L/R eines Streams): ihr Status
     * (buffer_position) aendert sich mit jedem Frame. */
    for (int i = 0; i < 4000; i++) wr16(g_mem + 0x14000000u + (unsigned)i * 2, (unsigned)i);
    for (int s = 0; s < 2; s++) {
        cfg = region(0) + SM_SOURCE_CONFIG + (unsigned)s * 192u;
        memset(cfg, 0, 192);
        wr32(cfg + CFG_DIRTY, (1u << 30) | (1u << 16));
        wr16(cfg + CFG_ENABLE, 1);
        wr16(cfg + CFG_EMB_FLAGS1, 1u | (1u << 2));  /* Mono, PCM16 */
        wr_dsp32(cfg + CFG_EMB_PHYS, 0x20000000u);
        wr_dsp32(cfg + CFG_EMB_LEN, 4000u);
        wr16(cfg + CFG_EMB_FLAGS2, 2u);              /* is_looping */
    }

    /* Takt laeuft ueber mehrere Perioden (4,9 ms), niemand fuehrt aus. */
    f0 = audio_dsp_frames();
    memcpy(snap[0], region(0), 0x8000);
    memcpy(snap[1], region(1), 0x8000);
    audio_dsp_start();
    Sleep(40);
    f1 = audio_dsp_frames();
    check(g_notified >= 3, "takt_meldet_faellige_frames");
    check(audio_dsp_due(), "frame_faellig");
    check(f1 == f0, "kein_frame_ohne_scheduler");
    check(memcmp(snap[0], region(0), 0x8000) == 0 && memcmp(snap[1], region(1), 0x8000) == 0,
          "regionen_unveraendert_ohne_scheduler");

    /* Scheduler fuehrt einen Frame aus: Status geschrieben, Pipe-Interrupt. */
    g_int_pipe = 0;
    audio_dsp_run_due();
    check(audio_dsp_frames() == f1 + 1, "run_due_ein_frame");
    check(g_int_pipe == 1, "run_due_meldet_interrupt");
    /* Spiel hat noch nicht geantwortet: Folgeframe wartet (Kopplung). */
    ran = 0;
    if (audio_dsp_due()) {
        audio_dsp_run_due();
        ran = audio_dsp_frames() != f1 + 1;
    }
    check(!ran, "folgeframe_wartet_auf_spielantwort");
    /* Spiel gibt die naechste Region frei -> Folgeframe laeuft sofort. */
    set_counter(1, 31);
    for (int i = 0; i < 100 && !audio_dsp_due(); i++) Sleep(2);
    audio_dsp_run_due();
    check(audio_dsp_frames() == f1 + 2, "folgeframe_nach_spielantwort");
    /* Ohne Antwort laeuft er spaetestens eine Periode nach der Zustellung. */
    Sleep(8);
    audio_dsp_run_due();
    check(audio_dsp_frames() == f1 + 3, "folgeframe_nach_einer_periode");

    audio_dsp_stop();
    audio_dsp_set_notify(NULL);
}

int main(void) {
    const uint32_t mem_size = 0x20000000u;
    unsigned char buf[4] = {0, 0, 0, 0};
    unsigned char *cfg, *st;

    g_mem = (unsigned char *)VirtualAlloc(NULL, mem_size, MEM_RESERVE | MEM_COMMIT,
                                          PAGE_READWRITE);
    if (g_mem == NULL) {
        printf("VirtualAlloc fehlgeschlagen\n");
        return 1;
    }
    audio_dsp_init(g_mem, mem_size);
    audio_dsp_set_interrupt(irq);

    /* Initialize: gesamten DSP-RAM (0x80000 ab 0x1FF00000) nullen. */
    g_mem[0x1FF00000u] = 0xAA;
    g_mem[0x1FF60000u] = 0xAA;
    audio_dsp_pipe_write(buf, 4);
    check(g_mem[0x1FF00000u] == 0 && g_mem[0x1FF60000u] == 0, "initialize_zero_ram");

    /* --- Regionsauswahl ueber den Frame-Counter --------------------- */
    set_counter(0, 5);
    set_counter(1, 3);
    cfg = region(0) + SM_SOURCE_CONFIG;
    st = region(1) + SM_SOURCE_STATUS;
    /* Sentinel in beiden Statusbereichen; nur die Schreibregion wird ersetzt. */
    region(0)[SM_SOURCE_STATUS + ST_ENABLED] = 0xEE;
    region(1)[SM_SOURCE_STATUS + ST_ENABLED] = 0xEE;
    wr32(cfg + CFG_DIRTY, (1u << 28) | (1u << 16));
    wr16(cfg + CFG_ENABLE, 1);
    wr16(cfg + CFG_SYNC_COUNT, 0x1234);
    audio_dsp_tick();
    check(region(0)[SM_SOURCE_STATUS + ST_ENABLED] == 0xEE, "read_region_untouched");
    check(region(1)[SM_SOURCE_STATUS + ST_ENABLED] == 0x00, "write_region_status");
    /* Quelle ohne Puffer wird nach dem Dequeue-Leerlauf abgeschaltet. */
    check(st[ST_SYNC] == 0x34 && st[ST_SYNC + 1] == 0x12, "status_sync_count");
    check(st[ST_DIRTY] == 1, "status_buffer_id_dirty");
    /* Dirty-Bits der gelesenen Konfiguration sind geloescht. */
    {
        uint32_t d = (uint32_t)cfg[CFG_DIRTY] | ((uint32_t)cfg[CFG_DIRTY + 1] << 8) |
                     ((uint32_t)cfg[CFG_DIRTY + 2] << 16) |
                     ((uint32_t)cfg[CFG_DIRTY + 3] << 24);
        check(d == 0, "source_dirty_cleared");
    }

    /* Wraparound: fc0=0xFFFF, fc1=0x0001 -> Region 1 ist die Lese-Region. */
    set_counter(0, 0xFFFF);
    set_counter(1, 0x0001);
    region(0)[SM_SOURCE_STATUS + ST_ENABLED] = 0xEE;
    region(1)[SM_SOURCE_STATUS + ST_ENABLED] = 0xEE;
    audio_dsp_tick();
    check(region(1)[SM_SOURCE_STATUS + ST_ENABLED] == 0xEE, "wrap_read_region_untouched");
    check(region(0)[SM_SOURCE_STATUS + ST_ENABLED] == 0x00, "wrap_write_region");

    /* --- PCM16-Puffer + u32_dsp-Status ------------------------------ */
    set_counter(0, 10);
    set_counter(1, 9);
    memset(region(1) + SM_SOURCE_STATUS, 0, 24 * 12);
    /* FCRAM phys 0x20000000+x liegt im Port bei virtuell 0x14000000+x
     * (Linear-Heap), nicht bei x (dort liegt Code). */
    check(audio_phys_to_guest(0x20000000u) == 0x14000000u, "phys_fcram_basis");
    check(audio_phys_to_guest(0x20123456u) == 0x14123456u, "phys_fcram_offset");
    check(audio_phys_to_guest(0x1FF50000u) == 0x1FF50000u, "phys_dsp_ram_identisch");
    check(audio_phys_to_guest_aligned(0x20000002u) == 0x14000000u, "phys_dma_ausgerichtet");
    /* Gast-Puffer an physikalisch 0x20000000 -> virtuell 0x14000000; 1000 Stereo-Frames. */
    for (int i = 0; i < 2000; i++) wr16(g_mem + 0x14000000u + i * 2, (unsigned)(i & 0x7FFF));
    cfg = region(0) + SM_SOURCE_CONFIG;
    memset(cfg, 0, 192);
    wr32(cfg + CFG_DIRTY, (1u << 30) | (1u << 16));
    wr16(cfg + CFG_ENABLE, 1);
    wr16(cfg + CFG_EMB_FLAGS1, 2u | (1u << 2)); /* Stereo, Format PCM16 */
    wr_dsp32(cfg + CFG_EMB_PHYS, 0x20000000u + 2u); /* unaligned: DMA maskiert */
    wr_dsp32(cfg + CFG_EMB_LEN, 1000u);
    wr16(cfg + CFG_EMB_FLAGS2, 0);
    wr_dsp32(cfg + CFG_EMB_LEN, 1000u);
    audio_dsp_tick();   /* Frame 1: Dequeue (Azahar liefert dabei keine Samples) */
    audio_dsp_tick();   /* Frame 2: 160 Samples verbrauchen */
    st = region(1) + SM_SOURCE_STATUS;
    check(st[ST_ENABLED] == 1, "pcm16_enabled");
    /* Azahar StepOverSamples: 160 Ausgaben verbrauchen 159 Eingaben
     * (inputi = input.size()-2 endet bei 159). u32_dsp 159 = 00 00 9F 00. */
    check(st[ST_POS] == 0x00 && st[ST_POS + 1] == 0x00 && st[ST_POS + 2] == 0x9F &&
              st[ST_POS + 3] == 0x00,
          "status_buffer_position_u32_dsp");
    /* Erste dekodierte Probe (Adresse maskiert): Sample 0 = 0, Sample 1 = 1. */
    check(st[ST_DIRTY] == 0, "embedded_buffer_not_from_queue");

    /* --- Loopender PCM16: Spiel aktualisiert den Rest live --------------
     * Azahar 5a1c580d6 (Source::GenerateFrame): bei loopendem PCM16 wird der
     * noch nicht verbrauchte Rest jeden Frame neu aus dem Gast-Speicher
     * dekodiert. Ohne diese Aktualisierung spielte der DSP die alten Bytes
     * weiter (hier 1160 statt der vom Spiel geschriebenen 3000). */
    buf[0] = 0;                       /* Initialize: RAM null, Quellen reset */
    audio_dsp_pipe_write(buf, 4);
    set_counter(0, 20);
    set_counter(1, 19);               /* read = Region 0, write = Region 1 */

    /* Master-Volume 1.0, Ausgabe Stereo (sonst bleibt der Endmix 0). */
    {
        unsigned char *dcfg = region(0) + 0x2860u;
        union { float f; uint32_t u; } mv;
        mv.f = 1.0f;
        wr32(dcfg + 4u, mv.u);
        wr16(dcfg + 22u, 1);             /* OutputFormat::Stereo */
        wr32(dcfg + 0u, (1u << 16) | (1u << 26));
    }

    /* 500 Stereo-Frames, L = 1000+i, R = 2000+i. */
    for (int i = 0; i < 500; i++) {
        wr16(g_mem + 0x14000000u + (unsigned)i * 4 + 0, (unsigned)(1000 + i));
        wr16(g_mem + 0x14000000u + (unsigned)i * 4 + 2, (unsigned)(2000 + i));
    }
    cfg = region(0) + SM_SOURCE_CONFIG;
    memset(cfg, 0, 192);
    wr32(cfg + CFG_DIRTY, (1u << 30) | (1u << 16) | (1u << 25));
    wr16(cfg + CFG_ENABLE, 1);
    wr16(cfg + CFG_EMB_FLAGS1, 2u | (1u << 2));  /* Stereo, PCM16 */
    wr_dsp32(cfg + CFG_EMB_PHYS, 0x20000000u);
    wr_dsp32(cfg + CFG_EMB_LEN, 500u);
    wr16(cfg + CFG_EMB_FLAGS2, 2u);              /* is_looping */
    {
        union { float f; uint32_t u; } g;
        g.f = 1.0f;
        wr32(cfg + 4u, g.u);                     /* gain0 links */
        wr32(cfg + 8u, g.u);                     /* gain0 rechts */
    }
    audio_dsp_tick();   /* Frame 1: Dequeue, gibt noch keine Samples aus */
    audio_dsp_tick();   /* Frame 2: 159 Samples verbraucht */
    st = region(1) + SM_SOURCE_STATUS;
    /* u32_dsp 159 = 00 00 9F 00. */
    check(st[ST_POS] == 0x00 && st[ST_POS + 1] == 0x00 && st[ST_POS + 2] == 0x9F &&
              st[ST_POS + 3] == 0x00,
          "loop_refresh_vorlauf");
    /* Das "Spiel" ueberschreibt den Rest ab Sample 160. */
    for (int i = 160; i < 500; i++) {
        wr16(g_mem + 0x14000000u + (unsigned)i * 4 + 0, 3000);
        wr16(g_mem + 0x14000000u + (unsigned)i * 4 + 2, 6000);
    }
    audio_dsp_tick();   /* Frame 3: Rest neu dekodiert */
    {
        const unsigned char *fs = region(1) + SM_FINAL_SAMPLES;
        int l0 = (int16_t)(unsigned)(fs[0] | (fs[1] << 8));
        int r0 = (int16_t)(unsigned)(fs[2] | (fs[3] << 8));
        int l2 = (int16_t)(unsigned)(fs[8] | (fs[9] << 8));
        int r2 = (int16_t)(unsigned)(fs[10] | (fs[11] << 8));
        /* Ausgabe 0/1 sind die zwei letzten alten Samples (Interpolations-
         * vorlauf), Ausgabe 2 ist Sample 160 aus dem neuen Rest. */
        check(l0 != l2, "loop_refresh_altes_sample");
        check(l2 == 3000 && r2 == 6000, "loop_refresh_neue_samples");
        check(r0 != r2, "loop_refresh_kanal_rechts");
    }

    /* --- Sleep: FinishPendingSleep im naechsten Frame, Reply +2 ----- */
    g_int_zero = 0;
    buf[0] = 3;
    audio_dsp_pipe_write(buf, 4);
    audio_dsp_tick();                 /* FinishPendingSleep, reply_frames=2 */
    check(audio_dsp_recv_data() == 1, "sleep_recv_data");
    audio_dsp_tick();                 /* 2 -> 1 */
    audio_dsp_tick();                 /* 1 -> 0, Interrupt Zero */
    check(g_int_zero == 1, "sleep_reply_two_frames");

    /* PipeWrite waehrend eines offenen Sleep schliesst ihn sofort ab. */
    buf[0] = 3;
    audio_dsp_pipe_write(buf, 4);
    buf[0] = 2;                       /* Wakeup */
    audio_dsp_pipe_write(buf, 4);
    check(audio_dsp_recv_data() == 0, "wakeup_after_pending_sleep");

    thread_frames_only_via_scheduler();

    VirtualFree(g_mem, 0, MEM_RELEASE);
    if (g_fail) {
        printf("%d FEHLER\n", g_fail);
        return 1;
    }
    printf("dsp_hle_selftest: OK\n");
    return 0;
}
