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
#include "vclock.h"

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

/* 5) Staffelstab: Wahl, FIFO in der Prioritaetsklasse, Yield ans Ende,
 *    Verdraengung nur durch strikt bessere Threads (push_front). */
static void test_baton(void) {
    /* 0 = Hauptthread 0x30, 1 = Render 0x1A, 2/3 = zwei Worker 0x31 */
    int prio[4] = {0x30, 0x1A, 0x31, 0x31};
    long long qseq[4] = {1, 2, 3, 4};
    unsigned char ready[4] = {1, 0, 1, 1};
    long long seq = 4, front = 0;
    int cur, next;

    cur = ks_pick_ready(prio, qseq, ready, 4, -1);
    CHECK(cur == 0, "bester bereiter: Hauptthread 0x30 vor 0x31");
    /* Render-Thread wird bereit: verdraengt den Hauptthread. */
    ready[1] = 1;
    next = ks_pick_ready(prio, qseq, ready, 4, cur);
    CHECK(next == 1 && ks_preempts(prio[next], prio[cur]), "0x1A verdraengt 0x30");
    qseq[cur] = --front;                 /* verdraengt: vorne in die Klasse */
    /* Render-Thread wartet wieder -> Hauptthread kommt zurueck. */
    ready[1] = 0;
    qseq[1] = ++seq;
    cur = ks_pick_ready(prio, qseq, ready, 4, -1);
    CHECK(cur == 0, "nach dem Warten: verdraengter Hauptthread zuerst");
    /* Gleichstand verdraengt nicht. */
    CHECK(!ks_preempts(prio[2], prio[3]), "gleiche Prioritaet verdraengt nicht");
    /* Hauptthread wartet: Worker 2/3 abwechselnd per Yield (Round-Robin). */
    ready[0] = 0;
    cur = ks_pick_ready(prio, qseq, ready, 4, -1);
    CHECK(cur == 2, "Worker 2 zuerst (frueher eingereiht)");
    qseq[cur] = ++seq;                   /* SleepThread(0): ans Ende */
    cur = ks_pick_ready(prio, qseq, ready, 4, -1);
    CHECK(cur == 3, "Yield: Worker 3 ist dran");
    qseq[cur] = ++seq;
    cur = ks_pick_ready(prio, qseq, ready, 4, -1);
    CHECK(cur == 2, "Yield: wieder Worker 2");
    /* Niemand bereit -> -1 (Leerlauf). */
    ready[2] = ready[3] = 0;
    CHECK(ks_pick_ready(prio, qseq, ready, 4, -1) == -1, "niemand bereit: Leerlauf");
}

/* 6) Thread-Ids eindeutig: ein endender Thread traegt nur SEINE Warte-
 *    eintraege aus (hle.c thread_signal_exit -> kwait_unregister_tid). Mit
 *    der frueheren Formel (n + 1) & 15 bekam der 16. Thread die Id 0 des
 *    Hauptthreads, sein Ende loeschte dessen Eintrag (Stillstand 2026-11-26). */
static void test_thread_ids(void) {
    enum { N = 200 };
    static uint32_t ids[N];
    int ok = 1;
    for (int n = 0; n < N; n++) {
        ids[n] = ks_thread_id(n);
        if (ids[n] == 0) ok = 0;                   /* 0 = Hauptthread */
        for (int k = 0; k < n; k++)
            if (ids[k] == ids[n]) ok = 0;
    }
    CHECK(ok, "Thread-Ids eindeutig und ungleich Hauptthread");

    /* Szenario: Hauptthread (Id 0) und Thread #0 warten; Thread #15 endet und
     * traegt seine Eintraege per Id aus. */
    {
        uint32_t waiter_tid[2] = {0u, ks_thread_id(0)};
        int used[2] = {1, 1};
        uint32_t exiting = ks_thread_id(15);
        for (int i = 0; i < 2; i++)
            if (used[i] && waiter_tid[i] == exiting) used[i] = 0;
        CHECK(used[0] && used[1], "Thread-Ende loescht keine fremden Warteeintraege");
    }
}

/* Virtuelle Zeit (--deterministic, vclock.h): exakte Ereigniszeiten, keine
 * kumulierte Rundung, Leerlauf springt nie ueber das naechste Ereignis. */
static void test_vclock(void) {
    VClock v;
    unsigned vbl = 0, dsp = 0;
    const uint64_t base = 1000000000ull;
    vclock_init(&v, base);
    /* VBlank 1 bei 4481136/268111856 s = 16713680 ns (abgerundet),
     * DSP-Frame 1 bei 1310720/268111856 s = 4888705 ns. */
    CHECK(vclock_event_ns(0, 1, VCLOCK_VBLANK_TICKS) == 16713680ull, "VBlank-Periode 16713680 ns");
    CHECK(vclock_event_ns(0, 1, VCLOCK_DSP_TICKS) == 4888705ull, "DSP-Periode 4888705 ns");
    /* 268111856 Ticks = genau 1 s: nach 59,83 VBlanks keine Drift. */
    CHECK(vclock_event_ns(0, VCLOCK_TICK_HZ, VCLOCK_VBLANK_TICKS) ==
              VCLOCK_VBLANK_TICKS * 1000000000ull, "VBlank-Zeitachse ohne Drift");
    CHECK(v.next_ns == base + 4888705ull, "erstes Ereignis ist der DSP-Frame");
    /* Leerlauf ohne Deadline: genau zum naechsten Ereignis. */
    vclock_idle_jump(&v, 0);
    CHECK(v.now_ns == base + 4888705ull, "Leerlauf springt zum DSP-Frame");
    vclock_collect(&v, &vbl, &dsp);
    CHECK(vbl == 0 && dsp == 1, "ein DSP-Frame faellig");
    /* Deadline vor dem naechsten Ereignis wird genau getroffen. */
    vclock_idle_jump(&v, base + 5000000ull);
    CHECK(v.now_ns == base + 5000000ull, "Leerlauf trifft eine fruehere Deadline");
    /* Deadline in der Vergangenheit: kein Rueckschritt, weiter zum Ereignis. */
    vclock_idle_jump(&v, base + 1ull);
    CHECK(v.now_ns == base + 2ull * 4888705ull + 1ull, "keine Zeit rueckwaerts, Fortschritt garantiert");
    /* Bis 1 s: 59 VBlanks und 204 DSP-Frames insgesamt. */
    {
        unsigned tv = 0, td = 1;
        while (v.next_ns <= base + 1000000000ull) {
            vclock_idle_jump(&v, 0);
            vclock_collect(&v, &vbl, &dsp);
            tv += vbl;
            td += dsp;
        }
        CHECK(tv == 59 && td == 204, "1 s virtuell: 59 VBlanks, 204 DSP-Frames");
    }
    /* Gleiche Schrittfolge -> gleiche Ereignisfolge (Reproduzierbarkeit). */
    {
        VClock a, b;
        uint64_t ha = 0, hb = 0;
        vclock_init(&a, base);
        vclock_init(&b, base);
        for (int i = 0; i < 100000; i++) {
            unsigned x, y;
            a.now_ns += VCLOCK_NS_PER_BLOCK * (uint64_t)(1 + (i % 7));
            b.now_ns += VCLOCK_NS_PER_BLOCK * (uint64_t)(1 + (i % 7));
            if (a.now_ns >= a.next_ns) { vclock_collect(&a, &x, &y); ha = ha * 31u + (uint64_t)i * 4u + x * 2u + y; }
            if (b.now_ns >= b.next_ns) { vclock_collect(&b, &x, &y); hb = hb * 31u + (uint64_t)i * 4u + x * 2u + y; }
        }
        CHECK(ha == hb && ha != 0, "gleiche Bloecke -> gleiche Ereignisfolge");
    }
}

int main(void) {
    test_priority();
    test_vclock();
    test_ticks();
    test_wait_index();
    test_wake_order();
    test_baton();
    test_thread_ids();
    if (fails) {
        printf("%d Fehler\n", fails);
        return 1;
    }
    printf("OK\n");
    return 0;
}
