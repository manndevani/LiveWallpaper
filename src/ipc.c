/* ipc.c - see ipc.h. */
#include "common.h"
#include "ipc.h"

HWND engine_window(void) { return FindWindowW(ENGINE_CLASS, NULL); }

/* The engine packs its answer into the LRESULT:
   bits 0-7 status, 8-23 fps, 24-39 width, 40-55 height, 56-57 speed guard level. */
EngineInfo engine_query(void)
{
    EngineInfo info;
    DWORD_PTR r = 0;
    HWND h = engine_window();
    memset(&info, 0, sizeof info);
    if (h && SendMessageTimeoutW(h, RegisterWindowMessageW(MSG_STATUS), 0, 0, SMTO_ABORTIFHUNG, 300, &r)) {
        info.status = (EngineStatus)(r & 0xFF);
        info.fps = (UINT)((r >> 8) & 0xFFFF);
        info.w = (UINT)((r >> 24) & 0xFFFF);
        info.h = (UINT)((r >> 40) & 0xFFFF);
        info.eased = (UINT)((r >> 56) & 0x3);
    }
    return info;
}

void engine_reload(void)
{
    HWND h = engine_window();
    if (h)
        PostMessageW(h, RegisterWindowMessageW(MSG_RELOAD), 0, 0);
}

void engine_pause(int mode)
{
    HWND h = engine_window();
    if (h)
        PostMessageW(h, RegisterWindowMessageW(MSG_PAUSE), (WPARAM)mode, 0);
}

BOOL engine_start(void) { return engine_window() ? TRUE : spawn_self(L""); }

void engine_stop(void)
{
    HWND h = engine_window();
    DWORD pid = 0;
    HANDLE proc = NULL;
    if (!h)
        return;
    GetWindowThreadProcessId(h, &pid);
    if (pid)
        proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
    PostMessageW(h, WM_CLOSE, 0, 0);
    if (proc) { /* return only once it is gone, so a restart can follow at once */
        WaitForSingleObject(proc, 3000);
        CloseHandle(proc);
    }
}

const WCHAR *status_text(EngineStatus s)
{
    switch (s) {
    case ST_RUNNING: return L"Running";
    case ST_WAITING: return L"Waiting for desktop";
    case ST_PAUSED: return L"Paused";
    case ST_LOCKED: return L"Paused \x00B7 locked";
    case ST_DISPLAY_OFF: return L"Paused \x00B7 display off";
    case ST_BATTERY: return L"Paused \x00B7 on battery";
    case ST_COVERED: return L"Paused \x00B7 covered";
    case ST_ERROR: return L"File failed \x00B7 see log";
    case ST_STILL: return L"Picture \x00B7 drawn by Windows";
    default: return L"Not running";
    }
}

BOOL spawn_self(const WCHAR *args)
{
    static WCHAR cmd[MAX_PATH + 64];
    WCHAR exe[MAX_PATH];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wsprintfW(cmd, L"\"%s\" %s", exe, args);
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return FALSE;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return TRUE;
}
