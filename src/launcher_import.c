/* launcher_import.c -- importing a game from a CHD, as the launcher shows it.
 * SPDX-License-Identifier: MIT.  Copyright (c) 2026 VCThunder contributors.
 *
 * src/ingest.c reads the CHD; this runs it on a thread, queues what was
 * dropped on the exe, and draws the block on the Launch page. The window
 * (launcher.c) dispatches the timer, the buttons and the close.
 */
#include "launcher_page.h"
#include "launcher_model.h"
#include "launcher_capture.h"
#include "ingest.h"

#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* ---- importing a game from a CHD --------------------------------------------
 *
 * THE FRONT DOOR FOR SOMEONE WHO DOES NOT BUILD ANYTHING. src/ingest.c does the
 * work on a thread of its own; this window shows it on the Launch page, above
 * the card it is filling, and a timer moves the bar. A card whose game is not
 * there yet offers the import where its Play button would be, because a greyed
 * Play is a dead end for someone who has never seen this window. */
#define IMPORT_MAX_FILES 8

static struct {
    HANDLE thread;
    CRITICAL_SECTION lock;
    int lock_ready;
    volatile LONG cancel, finished;
    volatile LONG64 done, total;
    char line[256];             /* the importer's last sentence, under lock */
    char data_dir[MAX_PATH];
    wchar_t files[IMPORT_MAX_FILES][MAX_PATH];
    int nfiles, at, replace;
    ingest_result_t result;
    ingest_ident_t ident;
    char why[1024];
    char results[2048];         /* what the finished imports said */
} s_import;

int import_running(void)
{
    return s_import.thread != NULL;
}

static void import_say(void *ctx, const char *line)
{
    (void)ctx;
    EnterCriticalSection(&s_import.lock);
    snprintf(s_import.line, sizeof s_import.line, "%s", line);
    LeaveCriticalSection(&s_import.lock);
}

static void import_progress(void *ctx, uint64_t done, uint64_t total)
{
    (void)ctx;
    InterlockedExchange64(&s_import.total, (LONG64)total);
    InterlockedExchange64(&s_import.done, (LONG64)done);
}

static DWORD WINAPI import_main(LPVOID unused)
{
    ingest_ui_t ui = { import_say, import_progress, &s_import.cancel, NULL };
    (void)unused;
    s_import.result = ingest_file(s_import.files[s_import.at],
                                 s_import.data_dir, s_import.replace, 0, &ui,
                                 &s_import.ident, s_import.why,
                                 sizeof s_import.why);
    InterlockedExchange(&s_import.finished, 1);
    return 0;
}

static const char *import_file_name(void)
{
    static char name[MAX_PATH];
    const wchar_t *f = s_import.files[s_import.at], *slash = wcsrchr(f, L'\\');
    WideCharToMultiByte(CP_UTF8, 0, slash ? slash + 1 : f, -1, name,
                        sizeof name, NULL, NULL);
    return name;
}

static int import_start_current(void)
{
    s_import.cancel = s_import.finished = 0;
    s_import.done = s_import.total = 0;
    s_import.why[0] = s_import.line[0] = 0;
    s_import.thread = CreateThread(NULL, 0, import_main, NULL, 0, NULL);
    if (!s_import.thread) {
        snprintf(s_import.results, sizeof s_import.results,
                 "The import could not be started.");
        return 0;
    }
    SetTimer(launcher_window(), IMPORT_TIMER, 100, NULL);
    return 1;
}

/* Begin importing `n` files into the data folder the file names. The window
 * shows the Launch page, where the progress is. */
static void import_begin(const wchar_t (*files)[MAX_PATH], int n)
{
    int i;
    if (import_running() || n <= 0)
        return;
    if (!s_import.lock_ready) {
        InitializeCriticalSection(&s_import.lock);
        s_import.lock_ready = 1;
    }
    /* Into the folder the game will be read from, so an unsaved change to it
     * is saved first, exactly as Play does. */
    if (!launcher_save_if_dirty())
        return;
    snprintf(s_import.data_dir, sizeof s_import.data_dir, "%s",
             model_value("paths", "data_dir"));
    for (i = 0; i < n && i < IMPORT_MAX_FILES; i++)
        wcscpy(s_import.files[i], files[i]);
    s_import.nfiles = i;
    s_import.at = 0;
    s_import.replace = 0;
    s_import.results[0] = 0;
    import_start_current();
    show_page(PAGE_HOME);
}

/* The CHDs to import, from the system's own file dialog. */
void import_pick(void)
{
    static wchar_t buf[32768];
    static wchar_t picked[IMPORT_MAX_FILES][MAX_PATH];
    OPENFILENAMEW ofn;
    wchar_t *p;
    int n = 0;

    if (import_running())
        return;
    memset(&ofn, 0, sizeof ofn);
    buf[0] = 0;
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = launcher_window();
    ofn.lpstrFilter = L"MAME CHDs and CD images (*.chd, *.cue, *.toc, *.iso, "
                      L"*.bin)\0*.chd;*.cue;*.toc;*.iso;*.bin\0"
                      L"All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = sizeof buf / sizeof buf[0];
    ofn.lpstrTitle = L"Import a game from your MAME CHD";
    ofn.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn))
        return;
    /* One file: its full path. Several: the folder, then each name. */
    p = buf + wcslen(buf) + 1;
    if (!*p) {
        wcsncpy(picked[n++], buf, MAX_PATH - 1);
    } else {
        for (; *p && n < IMPORT_MAX_FILES; p += wcslen(p) + 1)
            _snwprintf(picked[n++], MAX_PATH, L"%ls\\%ls", buf, p);
    }
    import_begin((const wchar_t (*)[MAX_PATH])picked, n);
}

/* The CHDs beside VCThunder.exe, for each game not imported yet: 1 when it
 * started importing them. */
int import_beside_exe(void)
{
    static wchar_t found[IMPORT_MAX_FILES][MAX_PATH];
    wchar_t own[MAX_PATH];
    int n;

    if (import_running())
        return 0;
    ingest_own_folder(own, MAX_PATH);
    n = ingest_find(own, model_value("paths", "data_dir"), found,
                    IMPORT_MAX_FILES);
    if (n)
        import_begin((const wchar_t (*)[MAX_PATH])found, n);
    return n > 0;
}

static void append_result(const char *line)
{
    size_t used = strlen(s_import.results);
    snprintf(s_import.results + used, sizeof s_import.results - used, "%s%s",
             used ? "\r\n" : "", line);
}

/* The timer: move the bar, and when an import ends, say how and start the
 * next. Rebuilds the page at the end, so a card that can now play says so. */
void import_tick(void)
{
    char text[320];
    if (!import_running()) {
        KillTimer(launcher_window(), IMPORT_TIMER);
        return;
    }
    if (!s_import.finished) {
        LONG64 done = s_import.done, total = s_import.total;
        HWND bar = launcher_page() ? GetDlgItem(launcher_page(), IDC_IMPORT_BAR) : NULL;
        if (bar)
            SendMessage(bar, PBM_SETPOS,
                        total ? (WPARAM)(done * 1000 / total) : 0, 0);
        if (total)
            snprintf(text, sizeof text, "%s: %lld of %lld MB written",
                     import_file_name(), (long long)(done >> 20),
                     (long long)(total >> 20));
        else {
            EnterCriticalSection(&s_import.lock);
            snprintf(text, sizeof text, "%s", s_import.line[0] ?
                     s_import.line : "Reading the CHD...");
            LeaveCriticalSection(&s_import.lock);
        }
        if (launcher_page() && !s_import.cancel)
            SetDlgItemTextA(launcher_page(), IDC_IMPORT_TEXT, text);
        return;
    }
    WaitForSingleObject(s_import.thread, INFINITE);
    CloseHandle(s_import.thread);
    s_import.thread = NULL;
    KillTimer(launcher_window(), IMPORT_TIMER);
    /* The title folder holds something else: ask, once, and only then. */
    if (s_import.result == INGEST_EXISTS && !s_import.replace) {
        char ask[1400];
        snprintf(ask, sizeof ask, "%s\n\nReplace those files with the game "
                 "from %s?", s_import.why, import_file_name());
        if (MessageBoxA(launcher_window(), ask, "Import from CHD",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES) {
            s_import.replace = 1;
            if (import_start_current()) {
                render_page();
                return;
            }
        }
    }
    append_result(s_import.why);
    s_import.replace = 0;
    if (s_import.result != INGEST_CANCELLED && ++s_import.at < s_import.nfiles &&
        import_start_current()) {
        render_page();
        return;
    }
    render_page();
}

/* Closing the window while an import runs stops it; the importer removes what
 * it had written. */
int import_stop_for_close(void)
{
    if (!import_running())
        return 1;
    if (MessageBoxA(launcher_window(), "A game is being imported. Stop the import and "
                    "close?", "Import from CHD",
                    MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return 0;
    InterlockedExchange(&s_import.cancel, 1);
    WaitForSingleObject(s_import.thread, INFINITE);
    CloseHandle(s_import.thread);
    s_import.thread = NULL;
    KillTimer(launcher_window(), IMPORT_TIMER);
    return 1;
}

/* Files given before the window opened: CHDs dropped on VCThunder.exe. */
static wchar_t s_queued[IMPORT_MAX_FILES][MAX_PATH];
static int s_nqueued;

void launcher_queue_import(const wchar_t *file)
{
    if (s_nqueued < IMPORT_MAX_FILES)
        wcsncpy(s_queued[s_nqueued++], file, MAX_PATH - 1);
}

void import_open(void)
{
    if (s_nqueued) {
        int n = s_nqueued;
        s_nqueued = 0;
        import_begin((const wchar_t (*)[MAX_PATH])s_queued, n);
    } else {
        import_beside_exe();
    }
}

void import_cancel(void)
{
    InterlockedExchange(&s_import.cancel, 1);
}

/* The Launch page's import block: running, a bar and a Cancel; finished, what
 * each import said. Returns where the cards start. */
int render_import_block(int y, int x, int w)
{
    if (import_running()) {
        char head[MAX_PATH + 32];
        HWND bar, cancel;
        int head_h;
        snprintf(head, sizeof head, "Importing %s%s", import_file_name(),
                 s_import.nfiles > 1 ? " (one of several)" : "");
        head_h = text_block_height(theme_font(), head, w - 120);
        make_control(0, "STATIC", head, SS_LEFT, x, y, w - 120, head_h,
                     launcher_page(), 0);
        cancel = make_control(0, "BUTTON", s_import.cancel ? "Stopping" :
                              "Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                              x + w - 100, y, 100, CTRL_H, launcher_page(),
                              IDC_IMPORT_CANCEL);
        EnableWindow(cancel, !s_import.cancel);
        y += max(head_h, CTRL_H) + 6;
        bar = make_control(0, PROGRESS_CLASSA, "", PBS_SMOOTH, x, y, w, 16,
                           launcher_page(), IDC_IMPORT_BAR);
        SendMessage(bar, PBM_SETRANGE32, 0, 1000);
        y += 22;
        make_control(0, "STATIC", "Reading the CHD...", SS_LEFT, x, y, w, 20,
                     launcher_page(), IDC_IMPORT_TEXT);
        return y + 32;
    }
    if (s_import.results[0]) {
        int h = text_block_height(theme_font(), s_import.results, w);
        make_control(0, "STATIC", s_import.results, SS_LEFT, x, y, w, h,
                     launcher_page(), IDC_IMPORT_TEXT);
        return y + h + 14;
    }
    return y;
}
