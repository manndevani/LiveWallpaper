/* lock.c - turning livewall's screen saver on and off in Windows.
 * See lock.h. The saver itself is in saver.c. */
#include "common.h"
#include "lock.h"

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
