/* Virtuelle Zeitbasis fuer reproduzierbare Laeufe (port.exe --deterministic).
 *
 * Alle gastsichtbaren Uhren des Kernels (svcGetSystemTick, Sleep-/Warte-
 * Deadlines, Kernel-Timer, HID-Abtastzeitpunkte, Y2R-Ende) lesen eine
 * gemeinsame Zeit `guest_ns()` (hle.c). Im Normalbetrieb ist das die
 * Host-Wandzeit (QPC) wie bisher; VBlank- und DSP-Takt laufen dann in
 * eigenen Host-Threads im Wandtakt.
 *
 * Im deterministischen Modus haengt die Zeit nur am Gastfortschritt:
 *
 *   * je ausgefuehrtem Gast-Block VCLOCK_NS_PER_BLOCK Nanosekunden
 *     (grobe Naeherung an ARM11-Zyklen; nur Reproduzierbarkeit zaehlt),
 *   * ist kein Gast-Thread bereit, springt die Zeit direkt zur naechsten
 *     Deadline (Leerlauf wartet nicht auf die Wanduhr),
 *   * VBlank (4481136 Ticks) und DSP-Frame (160 * 4096 * 2 = 1310720 Ticks,
 *     Azahar audio_core/hle/hle.cpp) sind Ereignisse auf dieser Zeitachse,
 *     ausgeloest vom Stabhalter, wenn die Zeit sie ueberschreitet.
 *
 * Damit liefern zwei Laeufe mit denselben Eingaben (an VBlank-Nummern
 * gebunden, siehe runtime/main.c) dieselbe Ereignisfolge im Gast. Das ist
 * ein Takt-Modus des Hosts; Gastspeicher und Spielcode bleiben unberuehrt.
 *
 * Reine Ganzzahl-Arithmetik ohne OS-Abhaengigkeit (tests/sched_selftest.c).
 */
#ifndef RUNTIME_KERNEL_VCLOCK_H
#define RUNTIME_KERNEL_VCLOCK_H

#include <stdint.h>

#define VCLOCK_TICK_HZ        268111856ull   /* ARM11 (Azahar core_timing.h) */
#define VCLOCK_VBLANK_TICKS   4481136ull     /* je VBlank (Azahar gpu.cpp) */
#define VCLOCK_DSP_TICKS      1310720ull     /* je DSP-Frame (160 Samples) */
#define VCLOCK_NS_PER_BLOCK   30ull          /* ~8 ARM11-Zyklen je Block */

/* Zeitpunkt (ns ab base) des n-ten Ereignisses mit Periode `ticks`. Exakt
 * (abgerundet) ohne kumulierte Rundungsfehler. */
static inline uint64_t vclock_event_ns(uint64_t base, uint64_t n, uint64_t ticks) {
#if defined(__SIZEOF_INT128__)
    return base + (uint64_t)(((unsigned __int128)n * ticks * 1000000000ull) / VCLOCK_TICK_HZ);
#else
    return base + (uint64_t)(((long double)n * (long double)ticks * 1e9L) /
                             (long double)VCLOCK_TICK_HZ);
#endif
}

typedef struct {
    uint64_t base_ns;   /* Nullpunkt (Boot) */
    uint64_t now_ns;    /* aktuelle virtuelle Zeit */
    uint64_t vbl_n;     /* Nummer des naechsten VBlanks (ab 1) */
    uint64_t dsp_n;     /* Nummer des naechsten DSP-Frames (ab 1) */
    uint64_t next_ns;   /* fruehestes der beiden naechsten Ereignisse */
} VClock;

static inline void vclock_update_next(VClock *v) {
    uint64_t a = vclock_event_ns(v->base_ns, v->vbl_n, VCLOCK_VBLANK_TICKS);
    uint64_t b = vclock_event_ns(v->base_ns, v->dsp_n, VCLOCK_DSP_TICKS);
    v->next_ns = a < b ? a : b;
}

static inline void vclock_init(VClock *v, uint64_t base_ns) {
    v->base_ns = base_ns;
    v->now_ns = base_ns;
    v->vbl_n = 1;
    v->dsp_n = 1;
    vclock_update_next(v);
}

/* Zieht alle bis now_ns faelligen Ereignisse ein: *vbl/*dsp erhalten die
 * Anzahl faelliger VBlanks bzw. DSP-Frames. */
static inline void vclock_collect(VClock *v, unsigned *vbl, unsigned *dsp) {
    unsigned a = 0, b = 0;
    while (v->now_ns >= vclock_event_ns(v->base_ns, v->vbl_n, VCLOCK_VBLANK_TICKS)) {
        v->vbl_n++;
        a++;
    }
    while (v->now_ns >= vclock_event_ns(v->base_ns, v->dsp_n, VCLOCK_DSP_TICKS)) {
        v->dsp_n++;
        b++;
    }
    vclock_update_next(v);
    *vbl = a;
    *dsp = b;
}

/* Leerlauf: Zeit auf `deadline` vorziehen, aber nie ueber das naechste
 * VBlank-/DSP-Ereignis hinaus und nie rueckwaerts. deadline == 0 = keine. */
static inline void vclock_idle_jump(VClock *v, uint64_t deadline) {
    uint64_t t = v->next_ns;
    if (deadline != 0 && deadline > v->now_ns && deadline < t) t = deadline;
    if (t > v->now_ns) v->now_ns = t;
}

#endif
