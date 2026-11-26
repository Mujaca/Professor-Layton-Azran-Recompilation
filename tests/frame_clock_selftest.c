/* Selbsttest der Host-Taktberechnung und der Fenstergeometrie.
 *
 * Reine Ganzzahl-Checks gegen in Python (Fraction) vorberechnete Werte:
 * VBlank-Rate 268111856/4481136 Hz, ein VBlank = 4481136/268111856 s.
 * Dazu der --press-Parser (runtime/platform/press_spec.h): alte Host-Frame-
 * Syntax unveraendert, Praefix g = Gast-Frames.
 * Kein Spielcode, keine Windows-Wartefunktion wird aufgerufen.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "frame_clock.h"
#include "press_spec.h"
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

    {
        /* --press: alte Syntax (Host-Frames) und Gast-Praefix g. */
        PressEv ev[PRESS_MAX];
        int n = press_spec_parse("start@300:10,a@600,touch@9000:20:160:120,"
                                 "touch@g1200:20:12:34,b@G50,kaputt,x@,y@-3,"
                                 "a@5:0,a@5:1:2:3:4:5", ev, 0, PRESS_MAX);
        check_int("press.n", n, 5);
        check_int("press0.at", ev[0].at, 300);
        check_int("press0.dur", ev[0].dur, 10);
        check_int("press0.guest", ev[0].guest, 0);
        check_int("press1.key_a", strcmp(ev[1].key, "a"), 0);
        check_int("press1.dur", ev[1].dur, 10);
        check_int("press2.tx", ev[2].tx, 160);
        check_int("press2.ty", ev[2].ty, 120);
        check_int("press3.guest", ev[3].guest, 1);
        check_int("press3.at", ev[3].at, 1200);
        check_int("press3.dur", ev[3].dur, 20);
        check_int("press3.tx", ev[3].tx, 12);
        check_int("press3.ty", ev[3].ty, 34);
        check_int("press4.guest", ev[4].guest, 1);
        check_int("press4.at", ev[4].at, 50);
        /* Aktiv-Fenster [at, at+dur): Host-Ereignis haengt nur am Host-,
           Gast-Ereignis nur am Gast-Zaehler. */
        check_int("press0.active299", press_ev_active(&ev[0], 299, 99999), 0);
        check_int("press0.active300", press_ev_active(&ev[0], 300, 0), 1);
        check_int("press0.active309", press_ev_active(&ev[0], 309, 0), 1);
        check_int("press0.active310", press_ev_active(&ev[0], 310, 0), 0);
        check_int("press3.host", press_ev_active(&ev[3], 1200, 0), 0);
        check_int("press3.g1200", press_ev_active(&ev[3], 0, 1200), 1);
        check_int("press3.g1219", press_ev_active(&ev[3], 0, 1219), 1);
        check_int("press3.g1220", press_ev_active(&ev[3], 0, 1220), 0);
        /* g<at>+<delay>: Anker beim Erreichen des Gast-Frames, danach
           Host-Frames (Standbild: Gast-Zaehler steht). */
        n = press_spec_parse("touch@g6000+120:6:97:57,a@g10+0,b@g5+x,c@g5:3+2", ev, 0, PRESS_MAX);
        check_int("pressd.n", n, 2);
        check_int("pressd0.at", ev[0].at, 6000);
        check_int("pressd0.delay", ev[0].delay, 120);
        check_int("pressd0.dur", ev[0].dur, 6);
        check_int("pressd0.tx", ev[0].tx, 97);
        check_int("pressd0.ty", ev[0].ty, 57);
        check_int("pressd1.delay", ev[1].delay, 0);
        check_int("pressd1.dur", ev[1].dur, 10);
        check_int("pressd0.h100g5999", press_ev_active(&ev[0], 100, 5999), 0);
        check_int("pressd0.h200g6000", press_ev_active(&ev[0], 200, 6000), 0);  /* Anker 200 */
        check_int("pressd0.anchor", ev[0].anchor, 200);
        check_int("pressd0.h319", press_ev_active(&ev[0], 319, 6000), 0);
        check_int("pressd0.h320", press_ev_active(&ev[0], 320, 6000), 1);
        check_int("pressd0.h325", press_ev_active(&ev[0], 325, 6000), 1);
        check_int("pressd0.h326", press_ev_active(&ev[0], 326, 6000), 0);
        check_int("pressd1.h50g10", press_ev_active(&ev[1], 50, 10), 1);

        /* Anhaengen bei mehreren --press-Angaben. */
        n = press_spec_parse("r@g7:3", ev, n, PRESS_MAX);
        check_int("press.append", n, 3);
        check_int("press2.at", ev[2].at, 7);
    }

    if (g_fail) {
        printf("FEHLGESCHLAGEN\n");
        return 1;
    }
    printf("OK\n");
    return 0;
}
