/* LaytonBuilder.exe - Starter des Builder-Pakets (Arbeitspaket 6).
 *
 * Kleines Win32-Programm: Nutzer waehlt seinen entschluesselten Dump
 * (Dateidialog, Drag & Drop auf Fenster oder exe, Kommandozeile), der Starter
 * ruft das mitgelieferte Python mit tools/builder.py auf und zeigt Log und
 * Fortschritt. Die gesamte Logik (Dumppruefung, Bauschritte) liegt in
 * tools/builder.py; hier nur Oberflaeche und Prozessstart.
 *
 * Paketlayout (tools/make_builder.py):
 *   <paket>/LaytonBuilder.exe
 *   <paket>/python/python.exe          eingebettetes Python
 *   <paket>/src/tools/builder.py       Bau-Treiber
 *   <paket>/game/                      Ausgabe (game/out/port.exe ...)
 *
 * Aufruf:
 *   LaytonBuilder.exe [dump.3ds] [--out <ordner>] [--headless]
 *   --headless: kein Fenster, baut sofort (Dump Pflicht), Exitcode des
 *   Builders; Log in <ordner>/builder.log und auf der Konsole des Aufrufers.
 *
 * Nutzertexte englisch (Paket fuer Endnutzer). Gebaut mit dem gebuendelten
 * zig (tools/make_builder.py): zig cc starter.c starter.rc -o LaytonBuilder.exe
 */
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define APP_TITLE L"Professor Layton and the Azran Legacy - Port Builder"
#define WM_APP_LINE (WM_APP + 1)
#define WM_APP_EXIT (WM_APP + 2)
#define ID_CHOOSE 101
#define ID_PLAY 102
#define ID_FOLDER 103
#define ID_TIMER 104
#define PATHN 1024

static wchar_t g_pkg[PATHN];      /* Ordner der exe */
static wchar_t g_game[PATHN];     /* Ausgabeordner */
static wchar_t g_dump[PATHN];
static int g_headless = 0;
static HWND g_wnd, g_log, g_status, g_bar, g_btn_choose, g_btn_play, g_btn_folder;
static HANDLE g_proc = NULL, g_job = NULL;
static volatile int g_running = 0;
static DWORD g_t0;
static wchar_t g_step[256] = L"";
static wchar_t g_error[1024] = L"";
static FILE *g_logf = NULL;
static HANDLE g_con = NULL;       /* Konsole des Aufrufers (headless) */

/* ---- Hilfen ---- */

static int file_exists(const wchar_t *p) {
    DWORD a = GetFileAttributesW(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void port_exe_path(wchar_t *out, size_t n) {
    _snwprintf(out, n, L"%ls\\out\\port.exe", g_game);
    out[n - 1] = 0;
}

static void log_write(const wchar_t *line) {
    char buf[4096];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, buf, sizeof(buf) - 2, NULL, NULL);
    if (n <= 0) return;
    buf[n - 1] = '\n';
    buf[n] = 0;
    if (g_logf) {
        fputs(buf, g_logf);
        fflush(g_logf);
    }
    if (g_con) {
        DWORD w;
        WriteFile(g_con, buf, (DWORD)n, &w, NULL);
    }
}

static void ui_append(const wchar_t *line) {
    if (!g_log) return;
    int len = GetWindowTextLengthW(g_log);
    SendMessageW(g_log, EM_SETSEL, len, len);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)line);
    SendMessageW(g_log, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
}

static void set_status(void) {
    wchar_t s[512];
    if (!g_status) return;
    if (g_running) {
        DWORD sec = (GetTickCount() - g_t0) / 1000;
        _snwprintf(s, 512, L"%ls   (%lu:%02lu elapsed)", g_step, sec / 60, sec % 60);
        s[511] = 0;
        SetWindowTextW(g_status, s);
    } else {
        SetWindowTextW(g_status, g_step);
    }
}

static void update_buttons(void) {
    wchar_t exe[PATHN];
    port_exe_path(exe, PATHN);
    if (!g_wnd) return;
    EnableWindow(g_btn_choose, !g_running);
    EnableWindow(g_btn_play, !g_running && file_exists(exe));
    EnableWindow(g_btn_folder, GetFileAttributesW(g_game) != INVALID_FILE_ATTRIBUTES);
}

/* Verknuepfung <game>/Play Professor Layton.lnk -> out/port.exe, Arbeits-
 * verzeichnis <game> (port.exe findet seine Daten auch ohne, siehe main.c). */
static const GUID k_clsid_shelllink = {0x00021401, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID k_iid_shelllinkw = {0x000214F9, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};
static const GUID k_iid_persistfile = {0x0000010B, 0, 0, {0xC0, 0, 0, 0, 0, 0, 0, 0x46}};

static void make_shortcut(void) {
    wchar_t exe[PATHN], lnk[PATHN];
    IShellLinkW *sl = NULL;
    IPersistFile *pf = NULL;
    port_exe_path(exe, PATHN);
    _snwprintf(lnk, PATHN, L"%ls\\Play Professor Layton.lnk", g_game);
    lnk[PATHN - 1] = 0;
    if (FAILED(CoInitialize(NULL))) return;
    if (SUCCEEDED(CoCreateInstance(&k_clsid_shelllink, NULL, CLSCTX_INPROC_SERVER,
                                   &k_iid_shelllinkw, (void **)&sl))) {
        sl->lpVtbl->SetPath(sl, exe);
        sl->lpVtbl->SetWorkingDirectory(sl, g_game);
        sl->lpVtbl->SetDescription(sl, L"Professor Layton and the Azran Legacy (native port)");
        if (SUCCEEDED(sl->lpVtbl->QueryInterface(sl, &k_iid_persistfile, (void **)&pf))) {
            pf->lpVtbl->Save(pf, lnk, TRUE);
            pf->lpVtbl->Release(pf);
        }
        sl->lpVtbl->Release(sl);
    }
    CoUninitialize();
}

static void play(void) {
    wchar_t exe[PATHN], cmd[PATHN + 8];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    port_exe_path(exe, PATHN);
    _snwprintf(cmd, PATHN + 8, L"\"%ls\"", exe);
    cmd[PATHN + 7] = 0;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, g_game, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        MessageBoxW(g_wnd, L"Could not start port.exe.", APP_TITLE, MB_OK | MB_ICONERROR);
    }
}

/* ---- Builder-Prozess ---- */

/* Eine Ausgabezeile des Builders (UTF-8) verarbeiten; im UI-Thread. */
static void handle_line(wchar_t *line) {
    log_write(line);
    if (wcsncmp(line, L"@step ", 6) == 0) {
        _snwprintf(g_step, 256, L"Step %ls", line + 6);
        g_step[255] = 0;
        set_status();
    } else if (wcsncmp(line, L"@progress ", 10) == 0) {
        if (g_bar) SendMessageW(g_bar, PBM_SETPOS, (WPARAM)_wtoi(line + 10), 0);
    } else if (wcsncmp(line, L"@error ", 7) == 0) {
        _snwprintf(g_error, 1024, L"%ls", line + 7);
        g_error[1023] = 0;
        ui_append(line + 7);
    } else if (wcsncmp(line, L"@done", 5) == 0) {
        /* Abschluss in on_exit */
    } else {
        ui_append(line);
    }
}

typedef struct {
    HANDLE rd;
} ReaderArg;

static void deliver(wchar_t *w) {
    if (g_headless) {
        handle_line(w);
        free(w);
    } else {
        PostMessageW(g_wnd, WM_APP_LINE, 0, (LPARAM)w);
    }
}

static DWORD WINAPI reader_thread(LPVOID p) {
    ReaderArg *ra = (ReaderArg *)p;
    char buf[4096], line[8192];
    size_t ln = 0;
    DWORD got;
    while (ReadFile(ra->rd, buf, sizeof(buf), &got, NULL) && got > 0) {
        for (DWORD i = 0; i < got; i++) {
            char c = buf[i];
            if (c == '\r') continue;
            if (c == '\n' || ln == sizeof(line) - 1) {
                line[ln] = 0;
                int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, NULL, 0);
                wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
                if (w) {
                    MultiByteToWideChar(CP_UTF8, 0, line, -1, w, n);
                    deliver(w);
                }
                ln = 0;
                if (c == '\n') continue;
            }
            line[ln++] = c;
        }
    }
    CloseHandle(ra->rd);
    free(ra);
    return 0;
}

/* Wartet auf Leser + Prozess und meldet das Ende an das Fenster. */
static DWORD WINAPI waiter_thread(LPVOID p) {
    HANDLE th = (HANDLE)p;
    WaitForSingleObject(th, INFINITE);
    CloseHandle(th);
    WaitForSingleObject(g_proc, INFINITE);
    PostMessageW(g_wnd, WM_APP_EXIT, 0, 0);
    return 0;
}

static int start_build(void) {
    wchar_t py[PATHN], script[PATHN], cmd[4 * PATHN + 64], logp[PATHN];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd, wr;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jl;

    _snwprintf(py, PATHN, L"%ls\\python\\python.exe", g_pkg);
    _snwprintf(script, PATHN, L"%ls\\src\\tools\\builder.py", g_pkg);
    if (!file_exists(py) || !file_exists(script)) {
        _snwprintf(g_error, 1024, L"The package is incomplete: %ls is missing. "
                   L"Please extract the whole ZIP again.", file_exists(py) ? script : py);
        return 0;
    }
    CreateDirectoryW(g_game, NULL);
    _snwprintf(logp, PATHN, L"%ls\\builder.log", g_game);
    if (g_logf) fclose(g_logf);
    g_logf = _wfopen(logp, L"wb");

    _snwprintf(cmd, 4 * PATHN + 64, L"\"%ls\" -u \"%ls\" build \"%ls\" --game \"%ls\"",
               py, script, g_dump, g_game);
    if (!CreatePipe(&rd, &wr, &sa, 0)) return 0;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    /* Job: Schliessen des Starters beendet Python samt zig-Kindprozessen. */
    g_job = CreateJobObjectW(NULL, NULL);
    memset(&jl, 0, sizeof(jl));
    jl.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (g_job) SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &jl, sizeof(jl));
    if (!CreateProcessW(py, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                        NULL, g_pkg, &si, &pi)) {
        CloseHandle(rd);
        CloseHandle(wr);
        _snwprintf(g_error, 1024, L"Could not start the bundled Python (error %lu).",
                   GetLastError());
        return 0;
    }
    if (g_job) AssignProcessToJobObject(g_job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(wr);
    g_proc = pi.hProcess;
    g_running = 1;
    g_t0 = GetTickCount();
    g_error[0] = 0;
    {
        wchar_t l[PATHN + 32];
        _snwprintf(l, PATHN + 32, L"Dump: %ls", g_dump);
        l[PATHN + 31] = 0;
        log_write(l);
        ui_append(l);
        _snwprintf(l, PATHN + 32, L"Output folder: %ls", g_game);
        l[PATHN + 31] = 0;
        log_write(l);
        ui_append(l);
    }
    ReaderArg *ra = (ReaderArg *)malloc(sizeof(ReaderArg));
    ra->rd = rd;
    HANDLE th = CreateThread(NULL, 0, reader_thread, ra, 0, NULL);
    if (g_headless) {
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
    } else {
        /* Ende melden, sobald Prozess beendet und Ausgabe gelesen ist. */
        HANDLE w = CreateThread(NULL, 0, waiter_thread, th, 0, NULL);
        if (w) CloseHandle(w);
    }
    return 1;
}

static DWORD finish_build(void) {
    DWORD rc = 1;
    WaitForSingleObject(g_proc, INFINITE);
    GetExitCodeProcess(g_proc, &rc);
    CloseHandle(g_proc);
    g_proc = NULL;
    if (g_job) {
        CloseHandle(g_job);
        g_job = NULL;
    }
    g_running = 0;
    {
        wchar_t exe[PATHN];
        port_exe_path(exe, PATHN);
        if (rc == 0 && file_exists(exe)) {
            make_shortcut();
            DWORD sec = (GetTickCount() - g_t0) / 1000;
            _snwprintf(g_step, 256, L"Done in %lu:%02lu. Click Play to start the game.",
                       sec / 60, sec % 60);
        } else {
            if (rc == 0) rc = 1;
            _snwprintf(g_step, 256, L"Build failed.");
            if (!g_error[0]) _snwprintf(g_error, 1024, L"The build failed (exit code %lu). "
                                        L"See builder.log in the output folder.", rc);
        }
    }
    log_write(g_step);
    if (g_logf) {
        fclose(g_logf);
        g_logf = NULL;
    }
    return rc;
}

static void begin(const wchar_t *dump) {
    wchar_t exe[PATHN];
    if (g_running) return;
    _snwprintf(g_dump, PATHN, L"%ls", dump);
    g_dump[PATHN - 1] = 0;
    port_exe_path(exe, PATHN);
    if (file_exists(exe) &&
        MessageBoxW(g_wnd, L"The game has already been built in this folder.\n\n"
                    L"Build it again from the selected dump? (Save games are kept.)",
                    APP_TITLE, MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    SetWindowTextW(g_log, L"");
    SendMessageW(g_bar, PBM_SETPOS, 0, 0);
    _snwprintf(g_step, 256, L"Starting ...");
    if (!start_build()) {
        MessageBoxW(g_wnd, g_error, APP_TITLE, MB_OK | MB_ICONERROR);
        return;
    }
    SetTimer(g_wnd, ID_TIMER, 1000, NULL);
    set_status();
    update_buttons();
}

static void choose(void) {
    wchar_t file[PATHN] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"3DS cartridge dump (*.3ds;*.cci)\0*.3ds;*.cci\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = PATHN;
    ofn.lpstrTitle = L"Select your decrypted dump of Professor Layton and the Azran Legacy (EUR)";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) begin(file);
}

/* ---- Fenster ---- */

static HFONT g_font;

static HWND mk(const wchar_t *cls, const wchar_t *txt, DWORD style, int id) {
    HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd,
                             (HMENU)(INT_PTR)id, GetModuleHandleW(NULL), NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, TRUE);
    return h;
}

static void layout(void) {
    RECT r;
    GetClientRect(g_wnd, &r);
    UINT dpi = 96;
    HMODULE u = GetModuleHandleW(L"user32.dll");
    typedef UINT(WINAPI * GetDpiForWindowFn)(HWND);
    GetDpiForWindowFn gd = u ? (GetDpiForWindowFn)(void *)GetProcAddress(u, "GetDpiForWindow") : NULL;
    if (gd) dpi = gd(g_wnd);
#define S(v) MulDiv((v), (int)dpi, 96)
    int w = r.right, h = r.bottom, m = S(10), bh = S(28), bw = S(130);
    MoveWindow(GetDlgItem(g_wnd, 200), m, m, w - 2 * m, S(48), TRUE);
    MoveWindow(g_btn_choose, m, m + S(52), bw, bh, TRUE);
    MoveWindow(g_btn_play, m + bw + m, m + S(52), bw, bh, TRUE);
    MoveWindow(g_btn_folder, m + 2 * (bw + m), m + S(52), bw, bh, TRUE);
    MoveWindow(g_bar, m, m + S(90), w - 2 * m, S(18), TRUE);
    MoveWindow(g_status, m, m + S(112), w - 2 * m, S(20), TRUE);
    MoveWindow(g_log, m, m + S(136), w - 2 * m, h - m - (m + S(136)), TRUE);
#undef S
}

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm;
        g_wnd = hw;
        ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        mk(L"STATIC", L"Select your decrypted dump (.3ds) of Professor Layton and the Azran "
                      L"Legacy (European release) - or drop it onto this window. The builder "
                      L"checks it and builds the game into the 'game' folder. Your dump is "
                      L"only read, never copied or changed.", 0, 200);
        g_btn_choose = mk(L"BUTTON", L"Choose dump...", BS_PUSHBUTTON, ID_CHOOSE);
        g_btn_play = mk(L"BUTTON", L"Play", BS_PUSHBUTTON, ID_PLAY);
        g_btn_folder = mk(L"BUTTON", L"Open game folder", BS_PUSHBUTTON, ID_FOLDER);
        g_bar = mk(PROGRESS_CLASSW, L"", 0, 0);
        SendMessageW(g_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        g_status = mk(L"STATIC", L"", SS_LEFTNOWORDWRAP, 0);
        g_log = mk(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                     ES_AUTOVSCROLL, 0);
        SendMessageW(g_log, EM_SETLIMITTEXT, 0, 0);
        DragAcceptFiles(hw, TRUE);
        {
            wchar_t exe[PATHN];
            port_exe_path(exe, PATHN);
            _snwprintf(g_step, 256, file_exists(exe)
                       ? L"The game is built. Click Play, or choose a dump to build it again."
                       : L"No game built yet. Choose your dump to start (the first build "
                         L"takes several minutes).");
        }
        layout();
        set_status();
        update_buttons();
        return 0;
    }
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO *)lp)->ptMinTrackSize.x = 520;
        ((MINMAXINFO *)lp)->ptMinTrackSize.y = 320;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_CHOOSE: choose(); break;
        case ID_PLAY: play(); break;
        case ID_FOLDER: ShellExecuteW(hw, L"open", g_game, NULL, NULL, SW_SHOWNORMAL); break;
        }
        return 0;
    case WM_DROPFILES: {
        wchar_t file[PATHN];
        HDROP d = (HDROP)wp;
        if (DragQueryFileW(d, 0, file, PATHN)) begin(file);
        DragFinish(d);
        return 0;
    }
    case WM_TIMER:
        set_status();
        return 0;
    case WM_APP_LINE: {
        wchar_t *w = (wchar_t *)lp;
        handle_line(w);
        free(w);
        return 0;
    }
    case WM_APP_EXIT: {
        DWORD rc;
        KillTimer(hw, ID_TIMER);
        rc = finish_build();
        if (rc == 0) SendMessageW(g_bar, PBM_SETPOS, 100, 0);
        set_status();
        update_buttons();
        if (rc == 0)
            MessageBoxW(hw, L"The game has been built.\n\nClick Play (or use the shortcut "
                        L"'Play Professor Layton' in the game folder) to start it.",
                        APP_TITLE, MB_OK | MB_ICONINFORMATION);
        else
            MessageBoxW(hw, g_error, APP_TITLE, MB_OK | MB_ICONERROR);
        return 0;
    }
    case WM_CLOSE:
        if (g_running &&
            MessageBoxW(hw, L"The build is still running. Cancel it and close?", APP_TITLE,
                        MB_YESNO | MB_ICONWARNING) != IDYES)
            return 0;
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        if (g_job) CloseHandle(g_job);   /* beendet einen laufenden Bau */
        g_job = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

/* ---- Einstieg ---- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const wchar_t *dump = NULL, *outdir = NULL;
    (void)prev;
    (void)cmdline;

    GetModuleFileNameW(NULL, g_pkg, PATHN);
    { wchar_t *s = wcsrchr(g_pkg, L'\\'); if (s) *s = 0; }
    for (int i = 1; i < argc; i++) {
        if (wcscmp(argv[i], L"--headless") == 0 || wcscmp(argv[i], L"--yes") == 0) g_headless = 1;
        else if (wcscmp(argv[i], L"--out") == 0 && i + 1 < argc) outdir = argv[++i];
        else dump = argv[i];
    }
    if (outdir) {
        if (!GetFullPathNameW(outdir, PATHN, g_game, NULL)) _snwprintf(g_game, PATHN, L"%ls", outdir);
    } else {
        _snwprintf(g_game, PATHN, L"%ls\\game", g_pkg);
    }
    { size_t n = wcslen(g_game); while (n > 3 && g_game[n - 1] == L'\\') g_game[--n] = 0; }
    if (dump) {
        wchar_t full[PATHN];
        if (GetFullPathNameW(dump, PATHN, full, NULL)) dump = _wcsdup(full);
    }

    if (g_headless) {
        DWORD rc;
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            g_con = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                                0, NULL);
            if (g_con == INVALID_HANDLE_VALUE) g_con = NULL;
        }
        if (!dump) {
            log_write(L"usage: LaytonBuilder.exe <dump.3ds> [--out <folder>] --headless");
            return 2;
        }
        _snwprintf(g_dump, PATHN, L"%ls", dump);
        if (!start_build()) {
            log_write(g_error);
            return 2;
        }
        rc = finish_build();
        if (rc != 0) log_write(g_error);
        return (int)rc;
    }

    {
        INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        WNDCLASSEXW wc;
        MSG m;
        InitCommonControlsEx(&icc);
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndproc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"LaytonBuilder";
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        RegisterClassExW(&wc);
        CreateWindowExW(WS_EX_ACCEPTFILES, L"LaytonBuilder", APP_TITLE, WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, 760, 520, NULL, NULL, inst, NULL);
        ShowWindow(g_wnd, show);
        UpdateWindow(g_wnd);
        if (dump) begin(dump);
        while (GetMessageW(&m, NULL, 0, 0) > 0) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    return 0;
}
