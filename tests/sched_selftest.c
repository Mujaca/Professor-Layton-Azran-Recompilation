/* Deterministischer Test der Scheduler-Entscheidungen aus
 * runtime/kernel/kernel_sched.h (Prioritaeten, Weck-Reihenfolge,
 * WaitSynchronizationN-Index, SystemTick/Timer).
 *
 *   python tests/sched_selftest.py
 *
 * Geprueft werden die reinen Entscheidungen, die hle.c benutzt; OS-Threads,
 * Gast-Lock und IPC sind nicht beteiligt (deshalb deterministisch).
 */
#include <stdio.h>
#include <stdint.h>

#include "kernel_sched.h"

static int fails = 0;

#define CHECK(cond, what)                                              \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("FAIL %s (Zeile %d)\n", (what), __LINE__);          \
            fails++;                                                   \
        }                                                              \
    } while (0)

/* 1) Prioritaeten: kleiner Wert = hoeher; Gleichstand = zuerst eingetragen. */
static void test_priority(void) {
    int prio[8] = {(int)KS_PRIO_DEFAULT, 0x18, (int)KS_PRIO_LOWEST, 0x18, 0, 0, 0, 0};
    unsigned char used[8] = {1, 1, 1, 1, 0, 0, 0, 0};

    CHECK(ks_pick_highest(prio, used, 8) == 1, "Gleichstand: frueherer Eintrag");
    used[1] = 0;
    CHECK(ks_pick_highest(prio, used, 8) == 3, "naechster Waiter derselben Prioritaet");
    {
        int p2[4] = {(int)KS_PRIO_LOWEST, 0, 0, 10};
        unsigned char u2[4] = {1, 0, 0, 1};
        CHECK(ks_pick_highest(p2, u2, 4) == 3, "unbenutzte Eintraege zaehlen nicht");
        CHECK(ks_pick_highest(p2, u2, 2) == 0, "nur Bereich 0..1 betrachtet");
    }

    /* App-Hauptthread 0x30 ist niedriger priorisiert als Worker 0x18. */
    CHECK(KS_PRIO_APP_MAX < KS_PRIO_DEFAULT && KS_PRIO_DEFAULT < KS_PRIO_LOWEST,
          "Prioritaetsordnung 0x18 < 0x30 < 0x3F");
}

/* 2) GetSystemTick: 268111856 Hz aus Host-Nanosekunden, ohne Ueberlauf. */
static void test_ticks(void) {
    CHECK(ks_tick_from_ns(0) == 0, "Tick 0");
    CHECK(ks_tick_from_ns(1000000000ull) == KS_TICK_HZ, "1 s = 268111856 Ticks");
    CHECK(ks_tick_from_ns(1000000ull) == 268111ull, "1 ms");
    CHECK(ks_tick_from_ns(200000000000ull) == 200ull * KS_TICK_HZ,
          "200 s ohne 64-Bit-Ueberlauf");

    uint64_t prev = 0;
    for (uint64_t ms = 0; ms <= 30000; ms += 17) {
        uint64_t t = ks_tick_from_ns(ms * 1000000ull);
        CHECK(t >= prev, "Ticks monoton");
        CHECK(ks_ns_from_tick(t) + 4 >= ms * 1000000ull &&
              ks_ns_from_tick(t) <= ms * 1000000ull + 4,
              "Tick -> ns konsistent (max. 4 ns Fehler)");
        prev = t;
    }
    /* Host-Zeit und Tick duerfen nicht auseinanderlaufen: 16-ms-VBlank. */
    uint64_t t0 = ks_tick_from_ns(1000ull * 1000000ull);
    uint64_t t1 = ks_tick_from_ns(1016ull * 1000000ull);
    CHECK(t1 - t0 == ks_tick_from_ns(16000000ull), "Differenz = 16 ms in Ticks");
}

/* 3) WaitSynchronizationN: Azahar sucht den Index rueckwaerts (rbegin). */
static void test_wait_index(void) {
    int a = 1, b = 2;
    const void *objs[3];
    objs[0] = &a;
    objs[1] = &b;
    objs[2] = &a;

    CHECK(ks_wait_index_last(objs, 3, 0) == 2, "doppeltes Objekt: letzter Index");
    CHECK(ks_wait_index_last(objs, 3, 1) == 1, "einfaches Objekt");
    CHECK(ks_wait_index_last(objs, 1, 0) == 0, "Liste mit einem Eintrag");
}

/* 4) Weck-Reihenfolge eines Objekts mit mehreren Waitern: Prioritaet, dann
 *    Eintragsreihenfolge (Azahar WakeupAllWaitingThreads). */
static void test_wake_order(void) {
    int prio[4] = {(int)KS_PRIO_DEFAULT, 0x18, 0x20, 0x18};
    unsigned char used[4] = {1, 1, 1, 1};
    int order[4];
    int n = 0;

    while (n < 4) {
        int k = ks_pick_highest(prio, used, 4);
        if (k < 0) break;
        order[n++] = k;
        used[k] = 0;
    }
    CHECK(n == 4, "alle Waiter werden geweckt");
    CHECK(order[0] == 1 && order[1] == 3 && order[2] == 2 && order[3] == 0,
          "Reihenfolge 0x18,0x18,0x20,0x30");
}

int main(void) {
    test_priority();
    test_ticks();
    test_wait_index();
    test_wake_order();
    if (fails) {
        printf("%d Fehler\n", fails);
        return 1;
    }
    printf("OK\n");
    return 0;
}
