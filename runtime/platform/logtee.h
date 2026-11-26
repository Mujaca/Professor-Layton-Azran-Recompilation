/* Konsolen-/Logausgabe des Ports (Arbeitspaket 7).
 *
 * port.exe ist ein Windows-Programm ohne eigene Konsole. Alles, was die
 * Runtime auf stdout/stderr schreibt, landet immer in einer Logdatei
 * (<exe-Ordner>/logs/port-JJJJMMTT-HHMMSS.log, die letzten LOG_KEEP bleiben)
 * und zusaetzlich
 *   - im urspruenglichen stdout/stderr, wenn der Aufrufer sie umgeleitet
 *     hat (Pipes der Tools: check.py, frontier.py, scene.py), und
 *   - in einem Konsolenfenster, wenn log_open_console() gerufen wurde
 *     (--console oder port.ini [debug] console = 1).
 * Reine Host-Ausgabe, kein Gastzustand. */
#ifndef LOGTEE_H
#define LOGTEE_H

#define LOG_KEEP 10

/* Ganz am Anfang von main(): Logdatei anlegen, stdout/stderr umleiten.
 * Rueckgabe 1 = Log aktiv. */
int log_init(void);
/* Konsole zeigen: an die Konsole des Aufrufers haengen (Start aus einem
 * Terminal) oder eine eigene oeffnen. Ohne Wirkung, wenn stdout schon in
 * eine Konsole geht. */
void log_open_console(void);
/* Pfad der aktuellen Logdatei ("" ohne Log). */
const char *log_path(void);

#endif
