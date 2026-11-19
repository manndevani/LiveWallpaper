/* desktop.c - see desktop.h and docs/FINDINGS.md for the window layouts. */
#include "common.h"
#include "desktop.h"
#include <dwmapi.h>

static HWND legacy_host;

static BOOL CALLBACK find_legacy(HWND h, LPARAM lp)
{
    (void)lp;
    if (FindWindowExW(h, NULL, L"SHELLDLL_DefView", NULL)) {
        legacy_host = FindWindowExW(NULL, h, L"WorkerW", NULL);
        return FALSE;
    }
    return TRUE;
}

HWND desktop_host(HWND *icons)
{
    DWORD_PTR r;
    HWND progman = FindWindowW(L"Progman", NULL);
    *icons = NULL;
    if (!progman)
        return NULL;

    /* Undocumented: makes Explorer split the wallpaper from the icon layer. */
    SendMessageTimeoutW(progman, 0x052C, 0xD, 0x1, SMTO_NORMAL, 1000, &r);
    SendMessageTimeoutW(progman, 0x052C, 0, 0, SMTO_NORMAL, 1000, &r);

    if (FindWindowExW(progman, NULL, L"WorkerW", NULL)) {
        /* Windows 11 24H2+: the icons and a WorkerW are children of Progman.
           We become a Progman child placed directly under the icon layer. */
        *icons = FindWindowExW(progman, NULL, L"SHELLDLL_DefView", NULL);
        return progman;
    }
    /* Older builds: the icons move to their own top-level WorkerW and an
       empty WorkerW appears behind it. That one is our canvas. */
    legacy_host = NULL;
    EnumWindows(find_legacy, 0);
    return legacy_host;
}

static BOOL CALLBACK add_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp)
{
    HRGN t = CreateRectRgnIndirect(r);
    (void)m;
    (void)dc;
    CombineRgn((HRGN)lp, (HRGN)lp, t, RGN_OR);
    DeleteObject(t);
    return TRUE;
}

static HWND cover_self;

static BOOL CALLBACK sub_window(HWND h, LPARAM lp)
{
    WCHAR cls[16];
    DWORD cloaked = 0;
    RECT r;
    HRGN t;
    int left;

    if (h == cover_self || !IsWindowVisible(h) || IsIconic(h))
        return TRUE;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TRANSPARENT)
        return TRUE; /* click-through overlays */
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked)) && cloaked)
        return TRUE; /* other virtual desktops, hidden UWP windows */
    if (GetClassNameW(h, cls, 16) && (!lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW")))
        return TRUE; /* the desktop itself */
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r)))
        GetWindowRect(h, &r);

    t = CreateRectRgnIndirect(&r);
    left = CombineRgn((HRGN)lp, (HRGN)lp, t, RGN_DIFF);
    DeleteObject(t);
    return left != NULLREGION; /* stop as soon as nothing is visible */
}

BOOL desktop_covered(HWND self)
{
    RECT box;
    BOOL covered;
    HRGN rgn = CreateRectRgn(0, 0, 0, 0);
    cover_self = self;
    EnumDisplayMonitors(NULL, NULL, add_monitor, (LPARAM)rgn);
    EnumWindows(sub_window, (LPARAM)rgn);
    covered = GetRgnBox(rgn, &box) == NULLREGION;
    DeleteObject(rgn);
    return covered;
}

BOOL desktop_under(POINT pt)
{
    WCHAR cls[16];
    HWND h = WindowFromPoint(pt), root;
    if (!h)
        return FALSE;
    root = GetAncestor(h, GA_ROOT);
    if (!root || !GetClassNameW(root, cls, 16))
        return FALSE;
    return !lstrcmpW(cls, L"Progman") || !lstrcmpW(cls, L"WorkerW");
}
