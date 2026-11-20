/* saver.c - livewall as a Windows screen saver.
 *
 *   livewall.scr /s           full screen, until a key, a click or the mouse moves
 *   livewall.scr /p <hwnd>    the small preview in Windows' Screen Saver Settings
 *   livewall.scr /c, no args  open livewall's settings on the Lock screen tab
 *
 * It draws the lock screen wallpaper (or the desktop one) with the same
 * sources as the engine. It never asks for a password: when "Ask for
 * password" is on, Windows locks the PC as this window closes, and the
 * normal Windows sign-in screen does the checking. */
#include "common.h"
#include "config.h"
#include "source.h"
#include "settings.h"
#include "lock.h"
#include <wtsapi32.h>

#define MOVE_PX 10     /* mouse movement that ends the screen saver */
#define GRACE_MS 600   /* ignore movement and keys right after starting (the Preview click, the shortcut keys) */

static Config cfg; /* large: keep it off the stack */

static const GUID kDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};

static struct {
    HWND wnd;
    BOOL preview;
    BOOL have_pos, display_off, animate, lost;
    BOOL trimmed;
    BOOL locking;  /* asked Windows to lock; waiting for it before closing */
    POINT start;
    DWORD started;
    const Source *src;
    SourceInfo info;
    LONGLONG qpf, last;
    double anim;
} v;

#define LOCK_WAIT_MS 3000 /* the most we stay up waiting for Windows to lock */

static void finish(void)
{
    HANDLE ev;
    if (v.preview || v.locking)
        return;
    /* Started by the lock shortcut: lock right now, and stay on screen until
       Windows reports the PC locked, so the desktop never shows in between
       (the engine also locks once we exit, as a backup). */
    ev = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, LOCK_EVENT);
    if (ev) {
        if (WaitForSingleObject(ev, 0) == WAIT_OBJECT_0) {
            ResetEvent(ev);
            if (LockWorkStation()) {
                log_line(L"screen saver: locking (lock shortcut)");
                v.locking = TRUE;
                if (v.src && v.src->play)
                    v.src->play(FALSE); /* freeze: leave the GPU to Windows' lock screen */
                SetTimer(v.wnd, 1, LOCK_WAIT_MS, NULL);
            } else {
                log_line(L"screen saver: could not lock (%u)", GetLastError());
            }
        }
        CloseHandle(ev);
    }
    if (!v.locking)
        PostMessageW(v.wnd, WM_CLOSE, 0, 0);
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_SETCURSOR:
        if (!v.preview) {
            SetCursor(NULL);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (!v.preview) {
            POINT p;
            GetCursorPos(&p);
            if (!v.have_pos || GetTickCount() - v.started < GRACE_MS) {
                v.start = p;
                v.have_pos = TRUE;
            } else if (p.x - v.start.x > MOVE_PX || v.start.x - p.x > MOVE_PX || p.y - v.start.y > MOVE_PX ||
                       v.start.y - p.y > MOVE_PX) {
                finish();
            }
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* Keys still held from the lock shortcut repeat; only a new press counts. */
        if ((l & (1 << 30)) || GetTickCount() - v.started < GRACE_MS)
            return 0;
        finish();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_MOUSEWHEEL:
        finish();
        return 0;
    case WM_ACTIVATEAPP:
        if (!w && GetTickCount() - v.started > GRACE_MS)
            finish();
        break;
    case WM_SYSCOMMAND:
        if (!v.preview && ((w & 0xFFF0) == SC_SCREENSAVE || (w & 0xFFF0) == SC_CLOSE))
            return 0;
        break;
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            const POWERBROADCAST_SETTING *ps = (const POWERBROADCAST_SETTING *)l;
            if (IsEqualGUID(&ps->PowerSetting, &kDisplayState) && ps->DataLength >= sizeof(DWORD))
                v.display_off = *(const DWORD *)ps->Data == 0;
        }
        return TRUE;
    case WM_WTSSESSION_CHANGE:
        if (v.locking && w == WTS_SESSION_LOCK) /* the lock screen is up: safe to go */
            PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_TIMER:
        if (w == 1) { /* no lock report in time: close anyway; the engine locks too */
            KillTimer(h, 1);
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void render(BOOL force)
{
    LARGE_INTEGER now;
    FrameInfo f;
    double dt;
    QueryPerformanceCounter(&now);
    dt = (double)(now.QuadPart - v.last) / (double)v.qpf;
    if (dt < 0 || dt > 0.25)
        dt = 0;
    v.last = now.QuadPart;
    v.anim += dt * cfg.speed / 100.0;

    memset(&f, 0, sizeof f);
    f.time = v.anim;
    f.phase = phase_at(v.anim);
    f.click_age = 1.0e4f;
    if (v.src->draw(&f, force)) {
        HRESULT hr;
        gfx_dim(1.0f - cfg.brightness / 100.0f);
        hr = gfx_present();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            v.lost = TRUE; /* keep the last frame; a screen saver can simply stop */
    }
}

static BOOL on_battery(void)
{
    SYSTEM_POWER_STATUS p;
    return GetSystemPowerStatus(&p) && p.ACLineStatus == 0;
}

/* Opens the lock screen wallpaper, or the first built-in if it fails. */
static BOOL open_source(void)
{
    const WCHAR *id = cfg.lock[0] ? cfg.lock : cfg.current;
    Kind k = kind_of(id);
    source_fit = cfg.fit;
    source_speed = cfg.speed / 100.0f;
    memset(&v.info, 0, sizeof v.info);
    if (k != KIND_UNKNOWN) {
        v.src = source_for(k);
        if (v.src->open(id, &v.info))
            return TRUE;
    }
    log_line(L"screen saver: could not show %s; showing %s instead", id, g_builtins[0].name);
    memset(&v.info, 0, sizeof v.info);
    v.src = &src_shader;
    return v.src->open(g_builtins[0].id, &v.info);
}

static HWND parse_hwnd(const WCHAR *s)
{
    ULONG_PTR n = 0;
    while (*s == ' ' || *s == ':')
        s++;
    for (; *s >= '0' && *s <= '9'; s++)
        n = n * 10 + (ULONG_PTR)(*s - '0');
    return (HWND)n;
}

static int run(HWND parent)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    WNDCLASSEXW wc;
    RECT r;
    HANDLE timer;
    HPOWERNOTIFY pn;
    LARGE_INTEGER q;
    UINT fps;
    BOOL ticking = FALSE;
    MSG msg;

    config_load(&cfg);
    QueryPerformanceFrequency(&q);
    v.qpf = q.QuadPart;
    v.preview = parent != NULL;
    v.started = GetTickCount();

    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = SAVER_CLASS;
    RegisterClassExW(&wc);

    if (parent) {
        GetClientRect(parent, &r);
        v.wnd = CreateWindowExW(0, SAVER_CLASS, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0, r.right, r.bottom,
                                parent, NULL, inst, NULL);
    } else {
        r.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
        r.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
        r.right = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        r.bottom = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        v.wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, SAVER_CLASS, APP, WS_POPUP | WS_VISIBLE, r.left,
                                r.top, r.right, r.bottom, NULL, NULL, inst, NULL);
        r.left = r.top = 0;
        ShowCursor(FALSE);
    }
    if (!v.wnd)
        return 1;
    if (!parent)
        SetForegroundWindow(v.wnd);
    pn = RegisterPowerSettingNotification(v.wnd, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!parent)
        WTSRegisterSessionNotification(v.wnd, NOTIFY_FOR_THIS_SESSION); /* see finish() */

    /* A screen saver can't do much about a missing GPU: the black window
       still covers the screen and Windows still locks afterwards. */
    if (gfx_init(cfg.low_power) && open_source() &&
        gfx_attach(v.wnd, (UINT)r.right, (UINT)r.bottom, v.info.full_res ? 1 : cfg.scale)) {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        v.last = now.QuadPart;
        /* Like the engine: on battery, show one still frame. */
        v.animate = v.info.animated && !(cfg.pause_battery && on_battery());
        if (v.src->play)
            v.src->play(v.animate);
        render(TRUE);
    } else {
        log_line(L"screen saver: no graphics, showing black");
        v.src = NULL;
    }

    fps = v.info.tick_fps ? max(v.info.tick_fps, cfg.fps) : cfg.fps;
    if (!fps)
        fps = 30;
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    for (;;) {
        BOOL want = v.src && v.animate && !v.locking /* freeze on the click */ && !v.display_off && !v.lost && timer;
        DWORD wr;
        if (want != ticking) {
            if (want) {
                LARGE_INTEGER due;
                due.QuadPart = -1;
                SetWaitableTimer(timer, &due, (LONG)(1000 / fps), NULL, NULL, FALSE);
            } else {
                CancelWaitableTimer(timer);
            }
            ticking = want;
        }
        wr = MsgWaitForMultipleObjectsEx(ticking ? 1 : 0, &timer, ticking || v.trimmed ? INFINITE : 5500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (ticking && wr == WAIT_OBJECT_0)
            render(FALSE);
        if (!v.trimmed && GetTickCount() - v.started > 5000) {
            /* Like the engine: most of what start-up needed is not needed again. */
            HeapCompact(GetProcessHeap(), 0);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            v.trimmed = TRUE;
        }
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
done:
    if (pn)
        UnregisterPowerSettingNotification(pn);
    if (!parent)
        WTSUnRegisterSessionNotification(v.wnd);
    if (timer)
        CloseHandle(timer);
    if (v.src)
        v.src->close();
    gfx_detach();
    gfx_free();
    if (!parent)
        ShowCursor(TRUE);
    return 0;
}

int saver_run(const WCHAR *arg, const WCHAR *rest)
{
    WCHAR c = arg[0] ? arg[1] : 'c';
    if (c == 's' || c == 'S')
        return run(NULL);
    if (c == 'p' || c == 'P') {
        HWND parent = parse_hwnd(arg[2] ? arg + 2 : rest);
        return IsWindow(parent) ? run(parent) : 1;
    }
    settings_run(SETTINGS_LOCK);
    return 0;
}
