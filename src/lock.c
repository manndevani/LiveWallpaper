/* lock.c - turning livewall's screen saver on and off in Windows.
 * See lock.h. The saver itself is in saver.c. */
#include "common.h"
#include "lock.h"
#include <tlhelp32.h>

#define DESKTOP_KEY L"Control Panel\\Desktop"
#define POLICY_KEY L"Software\\Policies\\Microsoft\\Windows\\Control Panel\\Desktop"

/* %LOCALAPPDATA%\livewall\livewall.scr. Windows runs a screen saver from a
   path stored in the registry, so it lives in a fixed place, not wherever
   the user unzipped livewall.exe. */
static void scr_path(WCHAR *out, BOOL make_dir)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) {
        out[0] = 0;
        return;
    }
    str_cat(out, L"\\livewall", MAX_PATH);
    if (make_dir)
        CreateDirectoryW(out, NULL);
    str_cat(out, L"\\livewall.scr", MAX_PATH);
}

static BOOL chosen_is_ours(void)
{
    WCHAR cur[MAX_PATH], mine[MAX_PATH];
    DWORD size = sizeof cur;
    scr_path(mine, FALSE);
    if (!mine[0] || RegGetValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"SCRNSAVE.EXE", RRF_RT_REG_SZ, NULL, cur,
                                 &size) != ERROR_SUCCESS)
        return FALSE;
    return str_ieq(cur, mine) || str_ieq(path_file(cur), L"livewall.scr");
}

void lock_query(LockState *s)
{
    BOOL active = FALSE, secure = FALSE;
    UINT timeout = 0;
    HKEY k;
    memset(s, 0, sizeof *s);
    SystemParametersInfoW(SPI_GETSCREENSAVEACTIVE, 0, &active, 0);
    SystemParametersInfoW(SPI_GETSCREENSAVETIMEOUT, 0, &timeout, 0);
    SystemParametersInfoW(SPI_GETSCREENSAVESECURE, 0, &secure, 0);
    /* Windows reports "active" even with "(None)" chosen, so also check
       which screen saver is set. */
    if (active) {
        if (chosen_is_ours())
            s->ours = TRUE;
        else {
            WCHAR cur[MAX_PATH];
            DWORD size = sizeof cur;
            s->other = RegGetValueW(HKEY_CURRENT_USER, DESKTOP_KEY, L"SCRNSAVE.EXE", RRF_RT_REG_SZ, NULL, cur,
                                    &size) == ERROR_SUCCESS && cur[0];
        }
    }
    s->secure = secure;
    s->minutes = timeout ? (timeout + 59) / 60 : 10;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, POLICY_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD n = 0;
        RegQueryInfoKeyW(k, NULL, NULL, NULL, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL);
        s->managed = n > 0;
        RegCloseKey(k);
    }
}

/* Same size and date: already up to date. */
static BOOL same_file(const WCHAR *a, const WCHAR *b)
{
    WIN32_FILE_ATTRIBUTE_DATA x, y;
    if (!GetFileAttributesExW(a, GetFileExInfoStandard, &x) || !GetFileAttributesExW(b, GetFileExInfoStandard, &y))
        return FALSE;
    return x.nFileSizeLow == y.nFileSizeLow && x.nFileSizeHigh == y.nFileSizeHigh &&
           CompareFileTime(&x.ftLastWriteTime, &y.ftLastWriteTime) == 0;
}

static BOOL copy_self(const WCHAR *scr)
{
    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH))
        return FALSE;
    if (str_ieq(exe, scr) || same_file(exe, scr))
        return TRUE;
    return CopyFileW(exe, scr, FALSE);
}

BOOL lock_apply(BOOL on, UINT minutes, BOOL secure)
{
    const UINT flags = SPIF_UPDATEINIFILE | SPIF_SENDCHANGE;
    WCHAR scr[MAX_PATH];
    HKEY k;
    BOOL ok = TRUE;

    scr_path(scr, on);
    if (!scr[0])
        return FALSE;
    if (on) {
        if (!copy_self(scr)) {
            log_line(L"screen saver: could not copy to %s (%u)", scr, GetLastError());
            return FALSE;
        }
        if (RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOP_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
            return FALSE;
        ok = RegSetValueExW(k, L"SCRNSAVE.EXE", 0, REG_SZ, (const BYTE *)scr,
                            (DWORD)(str_len(scr) + 1) * sizeof(WCHAR)) == ERROR_SUCCESS;
        RegCloseKey(k);
        ok = ok && SystemParametersInfoW(SPI_SETSCREENSAVETIMEOUT, (minutes ? minutes : 10) * 60, NULL, flags);
        ok = ok && SystemParametersInfoW(SPI_SETSCREENSAVESECURE, secure ? TRUE : FALSE, NULL, flags);
        ok = ok && SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, TRUE, NULL, flags);
    } else if (chosen_is_ours()) {
        /* Back to "(None)", as if it had never been set. */
        SystemParametersInfoW(SPI_SETSCREENSAVEACTIVE, FALSE, NULL, flags);
        if (RegOpenKeyExW(HKEY_CURRENT_USER, DESKTOP_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegDeleteValueW(k, L"SCRNSAVE.EXE");
            RegCloseKey(k);
        }
        SystemParametersInfoW(SPI_SETSCREENSAVESECURE, secure ? TRUE : FALSE, NULL, flags);
        DeleteFileW(scr); /* fails harmlessly while it is running */
    }
    log_line(L"screen saver %s (%u min, password %s)%s", on ? L"on" : L"off", minutes, secure ? L"on" : L"off",
             ok ? L"" : L": Windows refused");
    return ok;
}

void lock_refresh(void)
{
    WCHAR scr[MAX_PATH];
    LockState s;
    lock_query(&s);
    if (!s.ours)
        return;
    scr_path(scr, TRUE);
    if (scr[0] && copy_self(scr))
        return;
    log_line(L"screen saver: could not update %s", scr);
}

/* The lock shortcut. Asks Windows to start the screen saver now, the same
   way it does after the idle time, so Windows itself watches it and locks
   the PC when it ends; lock_check() also locks it the moment the screen saver
   closes, even inside Windows' grace period. Without livewall's screen saver,
   it simply locks. */
BOOL lock_now(void)
{
    LockState s;
    DWORD_PTR r;
    lock_query(&s);
    if (!s.ours || !s.secure) {
        log_line(L"lock shortcut: screen saver %s, locking directly", s.ours ? L"has no password" : L"is off");
        LockWorkStation();
        return FALSE; /* nothing to check afterwards */
    }
    SendMessageTimeoutW(GetDesktopWindow(), WM_SYSCOMMAND, SC_SCREENSAVE, 0, SMTO_ABORTIFHUNG, 1000, &r);
    log_line(L"lock shortcut: screen saver started");
    return TRUE;
}

/* The thread-pool wait on the shortcut's screen saver. Only this (engine)
   thread touches it: the previous one is released when the next is made. */
static HANDLE g_wait;

/* Runs on a thread-pool thread the moment the screen saver process ends.
   Windows skips the password if a screen saver ends within its grace period
   (ScreenSaverGracePeriod, 5 s by default). That suits an idle start, not a
   lock shortcut, so lock here whatever Windows decided. Locking an already
   locked PC does nothing. */
static VOID CALLBACK saver_ended(PVOID process, BOOLEAN timed_out)
{
    (void)timed_out;
    LockWorkStation(); /* no log here: log_line isn't thread-safe; the engine logs the lock */
    CloseHandle((HANDLE)process);
}

/* The running livewall.scr started by Windows. Windows runs screen savers on
   a desktop of their own, where FindWindow from ours can't see the window,
   so look for the process instead. */
static DWORD saver_pid(void)
{
    PROCESSENTRY32W pe;
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    HWND w;
    if (snap != INVALID_HANDLE_VALUE) {
        pe.dwSize = sizeof pe;
        if (Process32FirstW(snap, &pe))
            do {
                if (str_ieq(pe.szExeFile, L"livewall.scr"))
                    pid = pe.th32ProcessID; /* normally there is only one */
            } while (Process32NextW(snap, &pe));
        CloseHandle(snap);
    }
    if (!pid && (w = FindWindowW(SAVER_CLASS, NULL)) != NULL) /* started some other way */
        GetWindowThreadProcessId(w, &pid);
    return pid;
}

/* Called every 100 ms after lock_now, until it returns TRUE. Once the
   screen saver is running, waits on its process (see saver_ended). If it
   doesn't show up within 5 s, locks the normal way, so the shortcut always
   ends locked. */
BOOL lock_check(UINT waited_ms)
{
    DWORD pid = saver_pid();
    HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    if (p) {
        if (g_wait) { /* done, or pressed again while it shows: the new wait covers it */
            UnregisterWait(g_wait);
            g_wait = NULL;
        }
        if (RegisterWaitForSingleObject(&g_wait, p, saver_ended, p, INFINITE, WT_EXECUTEONLYONCE)) {
            log_line(L"lock shortcut: screen saver showing within %u ms", waited_ms);
            return TRUE;
        }
        CloseHandle(p);
        g_wait = NULL;
        log_line(L"lock shortcut: can't watch the screen saver (%u), locking directly", GetLastError());
        LockWorkStation();
        return TRUE;
    }
    if (waited_ms < 5000)
        return FALSE;
    log_line(L"lock shortcut: screen saver did not start, locking directly");
    LockWorkStation();
    return TRUE;
}
