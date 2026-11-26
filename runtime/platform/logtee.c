/* Konsolen-/Logausgabe, siehe logtee.h.
 *
 * Aufbau: stdout und stderr zeigen auf je eine anonyme Pipe; je ein
 * Lesethread schreibt alles in die Logdatei (gemeinsam, unter einer Sperre)
 * und reicht es an das urspruengliche Ziel (umgeleiteter stdout/stderr des
 * Aufrufers) sowie ggf. an die Konsole weiter. stdout bleibt ungepuffert
 * (main.c), jede printf-Zeile erreicht die Pipe sofort.
 *
 * Beenden: Die Pipes sind synchrone Handles; Windows arbeitet I/O auf
 * demselben Handle nacheinander ab, ein PeekNamedPipe wartete deshalb auf das
 * blockierende ReadFile des Lesethreads (Haenger beim Programmende). Statt zu
 * pollen werden beim Ende (atexit) und bei einem Absturz (Unhandled-
 * Exception-Filter) die Schreib-Enden geschlossen: der Lesethread liest den
 * Rest, bekommt dann "Pipe geschlossen" und endet; darauf wird kurz
 * gewartet. */
#include "logtee.h"

#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    HANDLE rd, wr;        /* Pipe: Runtime schreibt wr, Thread liest rd */
    HANDLE orig;          /* urspruengliches Ziel des Aufrufers oder NULL */
    HANDLE thread;
    FILE *fp;             /* stdout/stderr */
    int fd_pipe;          /* CRT-fd, dem wr gehoert */
} LogStream;

static CRITICAL_SECTION g_log_cs;
static HANDLE g_log_file = INVALID_HANDLE_VALUE;
static HANDLE g_con = NULL;               /* Konsole (CONOUT$) oder NULL */
static LogStream g_out, g_err;
static char g_log_path[MAX_PATH + 64] = "";
static volatile LONG g_active = 0;

static int handle_usable(HANDLE h) {
    return h != NULL && h != INVALID_HANDLE_VALUE &&
           GetFileType(h) != FILE_TYPE_UNKNOWN;
}

static void write_all(HANDLE h, const char *buf, DWORD n) {
    while (n > 0) {
        DWORD w = 0;
        if (!WriteFile(h, buf, n, &w, NULL) || w == 0) return;
        buf += w;
        n -= w;
    }
}

static DWORD WINAPI reader_main(LPVOID arg) {
    LogStream *s = (LogStream *)arg;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        /* Nur ein Fehler (Pipe geschlossen) beendet den Thread: ein
           Schreibvorgang mit 0 Bytes liefert ReadFile mit Erfolg und n == 0. */
        if (!ReadFile(s->rd, buf, sizeof(buf), &n, NULL)) break;
        if (n == 0) continue;
        EnterCriticalSection(&g_log_cs);
        if (g_log_file != INVALID_HANDLE_VALUE) write_all(g_log_file, buf, n);
        if (g_con != NULL) write_all(g_con, buf, n);
        LeaveCriticalSection(&g_log_cs);
        if (s->orig != NULL) write_all(s->orig, buf, n);
    }
    return 0;
}

/* <exe-Ordner>\logs */
static void log_dir(char *out, size_t n) {
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    char *slash;
    if (len == 0 || len >= sizeof(exe) || (slash = strrchr(exe, '\\')) == NULL) {
        snprintf(out, n, "logs");
        return;
    }
    *slash = '\0';
    snprintf(out, n, "%s\\logs", exe);
}

static int cmp_names(const void *a, const void *b) {
    return strcmp((const char *)a, (const char *)b);
}

/* Aeltere port-*.log loeschen, damit mit der neuen hoechstens LOG_KEEP
 * bleiben. Die Namen beginnen mit dem Zeitstempel und sortieren deshalb
 * chronologisch. */
static void rotate(const char *dir) {
    enum { MAXF = 512 };
    static char names[MAXF][MAX_PATH];
    char pat[MAX_PATH + 16];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    int n = 0, i;
    snprintf(pat, sizeof(pat), "%s\\port-*.log", dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n < MAXF)
            snprintf(names[n++], MAX_PATH, "%s", fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_names);
    for (i = 0; i + (LOG_KEEP - 1) < n; i++) {
        char p[MAX_PATH * 2];
        snprintf(p, sizeof(p), "%s\\%s", dir, names[i]);
        DeleteFileA(p);
    }
}

/* fd eines FILE-Streams auf die Pipe umbiegen. In einem Programm ohne
 * Konsole hat stdout anfangs keinen gueltigen fd; freopen auf NUL legt
 * einen an, den _dup2 dann ersetzt. */
static int redirect_stream(LogStream *s) {
    s->fd_pipe = _open_osfhandle((intptr_t)s->wr, _O_WRONLY | _O_TEXT);
    if (s->fd_pipe < 0) return 0;
    if (_fileno(s->fp) < 0) {
        if (freopen("NUL", "w", s->fp) == NULL) return 0;
    }
    return _dup2(s->fd_pipe, _fileno(s->fp)) == 0;
}

static int stream_init(LogStream *s, DWORD std_id, FILE *fp) {
    HANDLE orig = GetStdHandle(std_id);
    memset(s, 0, sizeof(*s));
    s->fp = fp;
    s->fd_pipe = -1;
    if (handle_usable(orig)) {
        HANDLE dup = NULL;
        if (DuplicateHandle(GetCurrentProcess(), orig, GetCurrentProcess(),
                            &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
            s->orig = dup;
    }
    if (!CreatePipe(&s->rd, &s->wr, NULL, 1u << 16)) return 0;
    if (!redirect_stream(s)) return 0;
    SetStdHandle(std_id, s->wr);
    s->thread = CreateThread(NULL, 0, reader_main, s, 0, NULL);
    return s->thread != NULL;
}

/* Schreib-Ende schliessen: der Stream zeigt danach auf NUL (spaete Ausgaben
 * anderer Threads gehen ins Leere statt in einen wiederverwendeten fd). */
static void stream_close(LogStream *s) {
    int nul;
    fflush(s->fp);
    nul = _open("NUL", _O_WRONLY);
    if (nul >= 0) {
        _dup2(nul, _fileno(s->fp));
        _close(nul);
    }
    if (s->fd_pipe >= 0) {
        _close(s->fd_pipe);       /* schliesst wr */
        s->fd_pipe = -1;
    }
}

static void finish(DWORD ms) {
    HANDLE t[2];
    if (InterlockedExchange(&g_active, 0) == 0) return;
    stream_close(&g_out);
    stream_close(&g_err);
    t[0] = g_out.thread;
    t[1] = g_err.thread;
    WaitForMultipleObjects(2, t, TRUE, ms);
}

static void at_exit(void) {
    finish(2000);
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    (void)ep;
    if (g_active) {
        fputs("\n[port] Absturz (unbehandelte Ausnahme) - Log endet hier\n", stdout);
        finish(500);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

int log_init(void) {
    char dir[MAX_PATH + 16];
    SYSTEMTIME st;
    InitializeCriticalSection(&g_log_cs);
    log_dir(dir, sizeof(dir));
    CreateDirectoryA(dir, NULL);
    rotate(dir);
    GetLocalTime(&st);
    snprintf(g_log_path, sizeof(g_log_path),
             "%s\\port-%04u%02u%02u-%02u%02u%02u-%lu.log", dir,
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             (unsigned long)GetCurrentProcessId());
    g_log_file = CreateFileA(g_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_log_file == INVALID_HANDLE_VALUE) g_log_path[0] = '\0';
    if (!stream_init(&g_out, STD_OUTPUT_HANDLE, stdout) ||
        !stream_init(&g_err, STD_ERROR_HANDLE, stderr)) {
        return 0;
    }
    g_active = 1;
    atexit(at_exit);
    SetUnhandledExceptionFilter(on_crash);
    return g_log_file != INVALID_HANDLE_VALUE;
}

void log_open_console(void) {
    HANDLE con;
    /* Geht stdout des Aufrufers schon in eine Konsole, ist sie sichtbar. */
    if (g_out.orig != NULL && GetFileType(g_out.orig) == FILE_TYPE_CHAR) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && !AllocConsole()) return;
    con = CreateFileA("CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                      NULL, OPEN_EXISTING, 0, NULL);
    if (con == INVALID_HANDLE_VALUE) return;
    EnterCriticalSection(&g_log_cs);
    g_con = con;
    LeaveCriticalSection(&g_log_cs);
}

const char *log_path(void) {
    return g_log_path;
}
