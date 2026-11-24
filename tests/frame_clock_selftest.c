/* Selbsttest der Host-Taktberechnung und der Fenstergeometrie.
 *
 * Reine Ganzzahl-Checks gegen in Python (Fraction) vorberechnete Werte:
 * VBlank-Rate 268111856/4481136 Hz, ein VBlank = 4481136/268111856 s.
 * Kein Spielcode, keine Windows-Wartefunktion wird aufgerufen.
 */
#include <stdint.h>
#include <stdio.h>

#include "frame_clock.h"
#include "window.h"

static int g_fail = 0;

static void check_u64(const char *what, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("FEHLER %s: got=%llu want=%llu\n", what,
               (unsigned long long)got, (unsigned long long)want);
        g_fail = 1;
    } else {
        printf("ok %s = %llu\n", what, (unsigned long long)got);
    }
}

static void check_int(const char *what, int got, int want) {
    if (got != want) {
        printf("FEHLER %s: got=%d want=%d\n", what, got, want);
        g_fail = 1;
    } else {
        printf("ok %s = %d\n", what, got);
    }
}

int main(void) {
    const uint64_t f10 = 10000000ull;
    WinViewport v;

    /* Rate: NUM Frames dauern genau DEN Sekunden. */
    check_u64("offset(NUM,1e9)", frame_clock_offset_qpc(FRAME_CLOCK_RATE_NUM,
                                                        1000000000ull),
              FRAME_CLOCK_RATE_DEN * 1000000000ull);
    /* Bei 10 MHz QPC: 4481136*1e7/268111856 = 167136,59... */
    check_u64("offset(1,10MHz)", frame_clock_offset_qpc(1, f10), 167136ull);
    check_u64("offset(2,10MHz)", frame_clock_offset_qpc(2, f10), 334273ull);
    check_u64("offset(60,10MHz)", frame_clock_offset_qpc(60, f10), 10028208ull);
    check_u64("offset(167151,10MHz)",
              frame_clock_offset_qpc(167151, f10), 27937084719ull);
    /* Eine Stunde: 215392 Frames, 35999931510 Ticks (68,5 ms vor 3600 s). */
    check_u64("offset(1h,10MHz)", frame_clock_offset_qpc(215392ull, f10),
              35999931510ull);
    check_u64("target(Anker=1000)",
              frame_clock_target_qpc(1000ull, 60, f10), 1000ull + 10028208ull);
    check_u64("period(10MHz)", frame_clock_period_qpc(f10), 167137ull);
    /* Monotonie: jede Frame-Zeit ist groesser als die vorherige. */
    {
        uint64_t prev = 0;
        int i, mono = 1;
        for (i = 0; i < 1000; i++) {
            uint64_t t = frame_clock_offset_qpc((uint64_t)i, f10);
            if (t <= prev && i > 0) mono = 0;
            prev = t;
        }
        check_int("monoton", mono, 1);
    }

    /* Fenstergeometrie: 400x480-Kanvas, ganzzahlig und zentriert. */
    v = win_fit_viewport(800, 960, 400, 480);
    check_int("vp800x960.scale", v.scale, 2);
    check_int("vp800x960.x", v.x, 0);
    check_int("vp800x960.y", v.y, 0);
    check_int("vp800x960.w", v.width, 800);
    check_int("vp800x960.h", v.height, 960);

    v = win_fit_viewport(800, 1080, 400, 480);
    check_int("vp800x1080.scale", v.scale, 2);
    check_int("vp800x1080.y", v.y, 60);

    v = win_fit_viewport(1920, 1080, 400, 480);
    check_int("vp1920x1080.scale", v.scale, 2);
    check_int("vp1920x1080.x", v.x, 560);

    v = win_fit_viewport(400, 480, 400, 480);
    check_int("vp400x480.scale", v.scale, 1);

    v = win_fit_viewport(320, 240, 400, 480);
    check_int("vp320x240.scale", v.scale, 1);
    check_int("vp320x240.x", v.x, -40);
    check_int("vp320x240.y", v.y, -120);

    if (g_fail) {
        printf("FEHLGESCHLAGEN\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
