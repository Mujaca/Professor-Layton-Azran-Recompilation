/* Parser fuer die Diagnose-Eingabe --press (runtime/main.c).
 *
 *   taste@frame[:dauer[:x:y]]     Host-Frames (Fenstertakt, wie bisher)
 *   taste@gframe[:dauer[:x:y]]    Gast-Frames (hle_gsp_guest_frames)
 *   taste@gframe+h[:dauer[:x:y]]  h Host-Frames, nachdem der Gast-Zaehler
 *                                 `frame` erreicht hat; Dauer in Host-Frames
 *
 * Mit dem Praefix `g` zaehlen Beginn UND Dauer in Gast-Frames, also in
 * VBlanks, in denen das Spiel einen eigenen Frame veroeffentlicht hat. Das ist
 * unter Last robust: laeuft der Gast langsamer als der Host, verschiebt sich
 * die Eingabe mit. Steht der Gast (Ladepause), bleibt eine laufende Eingabe
 * gedrueckt, bis er wieder Frames liefert. Wartet das Spiel auf einem
 * Standbild auf eine Eingabe, veroeffentlicht es keine neuen Frames und der
 * Gast-Zaehler steht - dafuer die Form `g<frame>+<host>`: verankert am
 * Gast-Fortschritt, die Wartezeit danach laeuft in Host-Frames.
 *
 * Reiner Host-Code ohne Abhaengigkeiten, damit tests/frame_clock_selftest.c
 * ihn direkt pruefen kann. Veraendert keinen Gastzustand.
 */
#ifndef PRESS_SPEC_H
#define PRESS_SPEC_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PRESS_MAX 64

typedef struct {
    char key[8];
    int at;      /* Beginn (Host- bzw. Gast-Frame) */
    int dur;     /* Dauer in derselben Einheit, Standard 10 */
    int tx, ty;  /* nur touch: unterer Screen, Standard 160,120 */
    int guest;   /* 1 = at in Gast-Frames */
    int delay;   /* >= 0: Form g<at>+<delay>, Start/Dauer in Host-Frames */
    int anchor;  /* Host-Frame, in dem der Gast-Zaehler `at` erreichte (-1) */
} PressEv;

/* Haengt die Ereignisse aus `spec` (kommagetrennt) an out[n..max) an und
 * liefert die neue Anzahl. Ungueltige Eintraege werden uebersprungen. */
static int press_spec_parse(const char *spec, PressEv *out, int n, int max) {
    const char *p = spec;
    while (p != NULL && *p != 0 && n < max) {
        char tok[96];
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len >= sizeof(tok)) len = sizeof(tok) - 1;
        memcpy(tok, p, len);
        tok[len] = 0;
        p = end ? end + 1 : NULL;
        {
            PressEv *e = &out[n];
            char *at = strchr(tok, '@');
            char *num;
            char tail;
            int got;
            if (at == NULL || at == tok) continue;
            *at = 0;
            memset(e, 0, sizeof(*e));
            snprintf(e->key, sizeof(e->key), "%s", tok);
            e->dur = 10;
            e->tx = 160;
            e->ty = 120;
            e->delay = -1;
            e->anchor = -1;
            num = at + 1;
            if (*num == 'g' || *num == 'G') {
                char *plus;
                e->guest = 1;
                num++;
                plus = strchr(num, '+');
                if (plus != NULL) {
                    char *colon = strchr(num, ':');
                    char *rest;
                    if (colon != NULL && colon < plus) continue;
                    e->delay = (int)strtol(plus + 1, &rest, 10);
                    if (rest == plus + 1 || e->delay < 0 || (*rest != 0 && *rest != ':')) continue;
                    *plus = 0;
                    if (*rest == ':') {
                        /* "at" + Rest ohne "+delay" erneut zusammensetzen */
                        memmove(plus, rest, strlen(rest) + 1);
                    }
                }
            }
            got = sscanf(num, "%d:%d:%d:%d%c", &e->at, &e->dur, &e->tx, &e->ty, &tail);
            if (got < 1 || got > 4 || e->at < 0 || e->dur <= 0) continue;
            n++;
        }
    }
    return n;
}

/* 1, wenn das Ereignis im aktuellen Host-/Gast-Frame gedrueckt ist. Einmal
 * je Host-Frame in aufsteigender Folge aufrufen (merkt sich den Anker der
 * Form g<at>+<delay>). */
static int press_ev_active(PressEv *e, int host_frame, unsigned guest_frame) {
    long long now;
    if (e->delay >= 0) {
        if (e->anchor < 0) {
            if ((long long)guest_frame < e->at) return 0;
            e->anchor = host_frame;
        }
        now = (long long)host_frame - e->anchor;
        return now >= e->delay && now < (long long)e->delay + e->dur;
    }
    now = e->guest ? (long long)guest_frame : (long long)host_frame;
    return now >= e->at && now < (long long)e->at + e->dur;
}

#endif
