/* Host-Bildtakt des 3DS: VBlank mit 268111856/4481136 Hz (59,8312 Hz).
 *
 * Der 3DS-Systemtick laeuft mit 268111856 Hz, ein VBlank-Abstand betraegt
 * 4481136 Ticks (3dbrew "GPU", Azahar video_core/gpu.cpp: 4481136 Ticks je
 * VBlank). Die Rate ist also 268111856/4481136 Hz, die Periode
 * 4481136/268111856 s = 16,71368 ms.
 *
 * Die Funktionen hier sind reine Ganzzahl-Arithmetik und damit in
 * tests/frame_clock_selftest.c ohne Windows-Abhaengigkeit pruefbar. Der
 * Warteteil nutzt QueryPerformanceCounter und einen hochaufloesenden
 * Waitable-Timer (CreateWaitableTimerExW mit
 * CREATE_WAITABLE_TIMER_HIGH_RESOLUTION); es wird nie busy gewartet.
 *
 * Der Takt haengt nur vom Host ab. Der Gast kann ihn weder verlangsamen noch
 * beschleunigen; ein einzelner langer Gast-Stall fuehrt zum Neuankern, nicht
 * zu einem Nachhol-Schwall.
 */
#ifndef FRAME_CLOCK_H
#define FRAME_CLOCK_H

#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <mmsystem.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002u
#endif
#endif

/* VBlank-Rate = NUM/DEN Hz, VBlank-Periode = DEN/NUM s. */
#define FRAME_CLOCK_RATE_NUM 268111856ull
#define FRAME_CLOCK_RATE_DEN 4481136ull

/* QPC-Ticks, die auf `frame` VBlank-Perioden entfallen (abgerundet). */
static inline uint64_t frame_clock_offset_qpc(uint64_t frame, uint64_t freq) {
#if defined(__SIZEOF_INT128__)
    return (uint64_t)(((__int128)frame * (__int128)FRAME_CLOCK_RATE_DEN *
                       (__int128)freq) /
                      (__int128)FRAME_CLOCK_RATE_NUM);
#else
    /* Fallback ohne 128 Bit (der Port baut mit zig cc/x86_64 und nimmt immer
     * den Zweig oben). Fuer sehr grosse frame-Werte ungenau, aber ohne
     * Ueberlauf und ohne Sprung in der Rate. */
    return (uint64_t)(((long double)frame * (long double)FRAME_CLOCK_RATE_DEN *
                       (long double)freq) /
                      (long double)FRAME_CLOCK_RATE_NUM);
#endif
}

/* Absoluter QPC-Zielzeitpunkt von Frame `frame` ab Anker `anchor`. */
static inline uint64_t frame_clock_target_qpc(uint64_t anchor, uint64_t frame,
                                              uint64_t freq) {
    return anchor + frame_clock_offset_qpc(frame, freq);
}

/* QPC-Ticks je VBlank-Periode (gerundet). */
static inline uint64_t frame_clock_period_qpc(uint64_t freq) {
    uint64_t a = frame_clock_offset_qpc(1, freq);
    uint64_t b = frame_clock_offset_qpc(2, freq);
    return b - a;
}

#if defined(_WIN32)

/* Ein FrameClock je Thread (eigener Timer); der globale
 * timeBeginPeriod(1) wird beim ersten Start einmal gesetzt. */
typedef struct {
    uint64_t freq;      /* QueryPerformanceFrequency */
    uint64_t anchor;    /* QPC-Zeit von Frame 0 */
    uint64_t frame;     /* naechster zu wartender Frame */
    HANDLE timer;       /* hochaufloesender Waitable-Timer */
    int started;
} FrameClock;

static inline void frame_clock_start(FrameClock *c) {
    static LONG period_set = 0;
    LARGE_INTEGER f, tt;
    memset(c, 0, sizeof(*c));
    if (InterlockedCompareExchange(&period_set, 1, 0) == 0) {
        timeBeginPeriod(1);   /* Fallback-Aufloesung fuer Sleep/Timer */
    }
    QueryPerformanceFrequency(&f);
    c->freq = (uint64_t)f.QuadPart;
    if (c->freq == 0) c->freq = 10000000ull;
    QueryPerformanceCounter(&tt);
    c->anchor = (uint64_t)tt.QuadPart;
    c->timer = NULL;
    {
        /* Erst der hochaufloesende Timer (Windows 10 1803+), sonst der
         * normale Waitable-Timer (von timeBeginPeriod(1) gestuetzt). */
        typedef HANDLE(WINAPI * CWTExW)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD,
                                        DWORD);
        CWTExW fn = (CWTExW)(void *)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "CreateWaitableTimerExW");
        if (fn != NULL) {
            c->timer = fn(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                          TIMER_ALL_ACCESS);
        }
        if (c->timer == NULL) {
            c->timer = CreateWaitableTimerW(NULL, FALSE, NULL);
        }
    }
    c->started = 1;
}

static inline uint64_t frame_clock_now_qpc(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}

/* Wartet bis zum naechsten VBlank-Ziel. Kein Busy-Wait: der Thread schlaeft
 * auf dem Timer; nach dem Aufwachen wird nur nachgeprueft (max. 4 Runden,
 * falls die Timer-Aufloesung frueher zurueckkehrt). */
static inline void frame_clock_wait(FrameClock *c) {
    uint64_t target, now, period, remaining;
    int i;
    if (!c->started) {
        frame_clock_start(c);
        return;
    }
    c->frame++;
    target = frame_clock_target_qpc(c->anchor, c->frame, c->freq);
    now = frame_clock_now_qpc();
    period = frame_clock_period_qpc(c->freq);
    if (now > target + period * 4ull) {
        /* Host war laenger als vier Frames blockiert: neu ankern, damit
         * nicht mehrere Frames ohne Pause nachgeholt werden. */
        c->anchor = now;
        c->frame = 0;
        return;
    }
    for (i = 0; i < 4 && c->timer != NULL; i++) {
        LONGLONG rel100;
        LARGE_INTEGER due;
        now = frame_clock_now_qpc();
        if (now >= target) break;
        remaining = target - now;
#if defined(__SIZEOF_INT128__)
        rel100 = (LONGLONG)(((( __int128)remaining * 10000000) +
                             (__int128)c->freq - 1) /
                            (__int128)c->freq);
#else
        rel100 = (LONGLONG)((remaining * 10000000ull + c->freq - 1) / c->freq);
#endif
        if (rel100 < 1) rel100 = 1;
        due.QuadPart = -rel100;
        if (!SetWaitableTimer(c->timer, &due, 0, NULL, NULL, FALSE)) break;
        WaitForSingleObject(c->timer, INFINITE);
    }
    /* Ohne Timer (exotische Umgebung): auf die naechste Millisekunde
     * schlafen, ohne zu drehen. */
    if (c->timer == NULL) {
        now = frame_clock_now_qpc();
        if (now < target) {
            uint64_t ms = (target - now) * 1000ull / c->freq;
            Sleep((DWORD)(ms ? ms : 1));
        }
    }
}

static inline void frame_clock_stop(FrameClock *c) {
    if (c->timer != NULL) {
        CloseHandle(c->timer);
        c->timer = NULL;
    }
    c->started = 0;
}

#endif /* _WIN32 */

#endif /* FRAME_CLOCK_H */
