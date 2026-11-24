/* Reine Entscheidungslogik des kooperativen Gast-Schedulers.
 *
 * Diese Datei enthaelt bewusst keine OS-Abhaengigkeiten: hle.c nutzt sie fuer
 * Prioritaets-/Tick-/Timer-Entscheidungen, tests/sched_selftest.c prueft sie
 * deterministisch. Semantik nach Azahar (nur als Referenz gelesen):
 *
 *   kernel/thread.h   ThreadPrioHighest = 0, ThreadPrioDefault = 48 (0x30),
 *                     ThreadPrioLowest = 63  -> kleiner Wert = hoehere Prio
 *   kernel/wait_object.cpp  GetHighestPriorityReadyThread: kleinste
 *                     current_priority gewinnt, bei Gleichstand der zuerst
 *                     eingetragene Thread
 *   kernel/thread.cpp GetWaitObjectIndex: sucht rueckwaerts (rbegin), liefert
 *                     bei doppelten Handles also den *letzten* Index
 *   core_timing.h     BASE_CLOCK_RATE_ARM11 = 268111856 Hz
 */
#ifndef RUNTIME_KERNEL_KERNEL_SCHED_H
#define RUNTIME_KERNEL_KERNEL_SCHED_H

#include <stdint.h>

/* Prioritaetsbereich des ARM11-Kernels (Azahar thread.h). */
#define KS_PRIO_HIGHEST   0u
#define KS_PRIO_APP_MAX   24u   /* hoechste Prioritaet fuer Userland-Apps */
#define KS_PRIO_DEFAULT   48u   /* 0x30: Hauptthread einer App (ExHeader) */
#define KS_PRIO_LOWEST    63u

/* GetSystemTick: ARM11-Takt (Azahar core_timing.h). */
#define KS_TICK_HZ 268111856ull

/* Waehlt unter den benutzten Eintraegen den mit der kleinsten Prioritaet
 * (kleiner = hoeher). Bei Gleichstand gewinnt der kleinere Index, weil die
 * Warteliste in Eintragsreihenfolge durchlaufen wird
 * (Azahar GetHighestPriorityReadyThread). Rueckgabe -1 = keiner. */
static inline int ks_pick_highest(const int *prio, const unsigned char *used, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!used[i]) continue;
        if (best < 0 || prio[i] < prio[best]) best = i;
    }
    return best;
}

/* Anzahl ARM11-Ticks seit demselben Nullpunkt wie `ns` (Nanosekunden).
 * Aufteilung in Sekunden/Rest verhindert den 64-Bit-Ueberlauf
 * (ns * 268111856 passt ab ca. 68 s nicht mehr in uint64_t). */
static inline uint64_t ks_tick_from_ns(uint64_t ns) {
    uint64_t sec = ns / 1000000000ull;
    uint64_t rem = ns % 1000000000ull;
    return sec * KS_TICK_HZ + (rem * KS_TICK_HZ) / 1000000000ull;
}

/* Tick-Differenz in Nanosekunden (fuer Diagnose/Tests). */
static inline uint64_t ks_ns_from_tick(uint64_t tick) {
    return (tick / KS_TICK_HZ) * 1000000000ull +
           ((tick % KS_TICK_HZ) * 1000000000ull) / KS_TICK_HZ;
}

/* Azahar Thread::GetWaitObjectIndex sucht rueckwaerts: bei mehrfachem
 * Vorkommen desselben Objekts in der Handle-Liste wird der letzte Index
 * gemeldet. `key` ist der Objektzeiger an Position `first`. */
static inline int ks_wait_index_last(const void *const *objs, int n, int first) {
    const void *key = objs[first];
    for (int i = n - 1; i >= 0; i--)
        if (objs[i] == key) return i;
    return first;
}

#endif /* RUNTIME_KERNEL_KERNEL_SCHED_H */
