/* engine.c - the always-running wallpaper process.
 *
 * Owns the window behind the desktop icons, the frame timer, the tray icon
 * and the pause rules. What gets drawn comes from a Source (source.h).
 * Single-threaded: the thread sleeps in MsgWaitForMultipleObjects until the
 * frame timer fires or a message arrives, and the timer is only armed while
 * the wallpaper is visible and animated. */
#include "common.h"
#include "config.h"
#include "autostart.h"
#include "ipc.h"
#include "desktop.h"
#include "source.h"
#include "engine.h"
#include "settings.h"
#include "still.h"
#include "lock.h"
#include <shellapi.h>
#include <wtsapi32.h>

#define WM_TRAY (WM_APP + 1)
#define POLL_MS 500
#define NO_CLICK 1.0e4f

enum { CMD_PAUSE = 1, CMD_EXIT, CMD_ADD, CMD_SETTINGS, CMD_AUTOSTART, CMD_BUILTIN = 100, CMD_LIBRARY = 200 };

static const GUID kDisplayState = {0x6fe69556, 0x704a, 0x47a0, {0x8f, 0x24, 0xc2, 0x8d, 0x93, 0x6f, 0xda, 0x47}};
static const GUID kPowerSource = {0x5d3e9a59, 0xe9d5, 0x4b00, {0xa6, 0xbd, 0xff, 0x34, 0xff, 0x51, 0x65, 0x48}};

static Config cfg; /* large: keep it off the stack */

static struct {
    HWND msg;   /* hidden top-level window: tray, IPC, power and session events */
    HWND wall;  /* our window behind the icons */
    HWND host;  /* the Explorer window it is a child of */
    HWND icons; /* icon layer we stay directly under (24H2+) */
    HICON icon;
    UINT wm_taskbar, wm_reload, wm_status, wm_pause;
    UINT lock_waited; /* ms since the lock shortcut started the screen saver */

    const Source *src;
    SourceInfo info;
    BOOL error; /* the chosen wallpaper failed, a built-in is showing */
    BOOL lost;  /* GPU device lost; rebuild at the next safe point */
    BOOL still; /* a still picture was handed to Windows: no device, no window */

    BOOL parked;            /* the source freed its decoder during a long pause */
    BOOL trimmed;           /* memory handed back since we last drew */
    BOOL settled;           /* startup-only memory handed back since drawing began */
    LONGLONG idle_since, trimmed_at, run_since;

    HANDLE timer;
    LONGLONG qpf, interval, due, last;
    double anim; /* seconds of animation */

    BOOL running, user_paused, covered, locked, display_off, on_battery;
    EngineStatus status;

    BOOL was_down, has_click;
    float click[2];
    double click_at;

    UINT frames, fps;  /* presented frames, and the rate over the last second */
    LONGLONG fps_since;

    /* Startup timing, until the window is first on the desktop: ms since
       the engine started (0 = not yet). See start_quiet(). */
    DWORD start_tick, gpu_ms, progman_ms;
    BOOL quiet_mode;
} e;

/* ---- tray -------------------------------------------------------------- */

static void tray(DWORD op)
{
    NOTIFYICONDATAW n;
    memset(&n, 0, sizeof n);
    n.cbSize = sizeof n;
    n.hWnd = e.msg;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    n.uCallbackMessage = WM_TRAY;
    n.hIcon = e.icon;
    wsprintfW(n.szTip, APP L" \x00B7 %s", status_text(e.status));
    if (!Shell_NotifyIconW(op, &n) && op == NIM_MODIFY)
        Shell_NotifyIconW(NIM_ADD, &n);
}

/* ---- timing ------------------------------------------------------------ */

static void arm_timer(LONGLONG due)
{
    LARGE_INTEGER now, rel;
    LONGLONG wait;
    QueryPerformanceCounter(&now);
    wait = due - now.QuadPart;
    if (wait < 0)
        wait = 0;
    rel.QuadPart = -(wait * 10000000 / e.qpf); /* relative, 100 ns units */
    SetWaitableTimerEx(e.timer, &rel, 0, NULL, NULL, NULL, 0);
}

static void update_interval(void)
{
    UINT fps = e.info.tick_fps ? max(e.info.tick_fps, cfg.fps) : cfg.fps;
    e.interval = e.qpf / (fps ? fps : 30);
}

static UINT want_scale(void) { return e.info.full_res ? 1 : cfg.scale; }

/* ---- drawing ----------------------------------------------------------- */

static void render(BOOL advance, BOOL force)
{
    LARGE_INTEGER now;
    FrameInfo f;
    double dt;

    if (!e.src || !gfx.sc)
        return;
    QueryPerformanceCounter(&now);
    if (advance) {
        dt = (double)(now.QuadPart - e.last) / (double)e.qpf;
        if (dt < 0 || dt > 0.25)
            dt = 0; /* after a pause, continue where we stopped */
        e.anim += dt * cfg.speed / 100.0;
    }
    e.last = now.QuadPart;

    memset(&f, 0, sizeof f);
    f.time = e.anim;
    f.phase = phase_at(e.anim);
    f.click_age = NO_CLICK;
    if (e.info.interactive && e.wall) {
        POINT pt, c;
        BOOL down;
        GetCursorPos(&pt);
        c = pt;
        ScreenToClient(e.wall, &c);
        f.mouse[0] = c.x * (float)gfx.bw / (float)gfx.ww;
        f.mouse[1] = c.y * (float)gfx.bh / (float)gfx.wh;
        f.mouse_on = desktop_under(pt) ? 1.0f : 0.0f;
        /* Poll the button instead of hooking the mouse: no system-wide hook. */
        down = (GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0;
        if (down && !e.was_down && f.mouse_on > 0) {
            e.click[0] = f.mouse[0];
            e.click[1] = f.mouse[1];
            e.click_at = e.anim;
            e.has_click = TRUE;
        }
        e.was_down = down;
        if (e.has_click) {
            f.click[0] = e.click[0];
            f.click[1] = e.click[1];
            f.click_age = (float)(e.anim - e.click_at);
        }
    }

    if (e.src->draw(&f, force)) {
        HRESULT hr;
        gfx_dim(1.0f - cfg.brightness / 100.0f);
        hr = gfx_present();
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
            e.lost = TRUE;
        else
            e.frames++;
    }
}

/* ---- state ------------------------------------------------------------- */

static void update_state(void)
{
    EngineStatus st;
    BOOL run;

    if (e.still)                                    st = ST_STILL;
    else if (!e.wall)                               st = ST_WAITING;
    else if (e.user_paused)                         st = ST_PAUSED;
    else if (e.locked)                              st = ST_LOCKED;
    else if (e.display_off)                         st = ST_DISPLAY_OFF;
    else if (e.on_battery && cfg.pause_battery)     st = ST_BATTERY;
    else if (e.covered && cfg.pause_covered)        st = ST_COVERED;
    else                                            st = ST_RUNNING;

    run = st == ST_RUNNING && e.src && e.info.animated;
    if (run != e.running) {
        if (run) {
            LARGE_INTEGER now;
            if (e.parked) {
                e.parked = FALSE;
                e.src->park(FALSE);
            }
            QueryPerformanceCounter(&now);
            e.last = e.due = e.run_since = now.QuadPart;
            e.settled = FALSE;
            arm_timer(e.due);
        } else {
            LARGE_INTEGER now;
            CancelWaitableTimer(e.timer);
            QueryPerformanceCounter(&now);
            e.idle_since = now.QuadPart;
        }
        e.running = run;
        e.trimmed = FALSE;
        if (e.src && e.src->play)
            e.src->play(run);
    }
    if (st == ST_RUNNING && e.src && !e.info.animated)
        st = ST_STILL;
    if (st == ST_RUNNING && e.error)
        st = ST_ERROR;
    if (st != e.status) {
        e.status = st;
        tray(NIM_MODIFY);
    }
}

static void wall_detach(void)
{
    HWND w = e.wall;
    gfx_detach();
    e.wall = NULL; /* clear first so wall_proc knows this is us */
    if (w)
        DestroyWindow(w);
    if (e.host && IsWindow(e.host))
        RedrawWindow(e.host, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    e.host = e.icons = NULL;
}

static BOOL wall_attach(void)
{
    RECT rc;
    HWND icons = NULL, host = desktop_host(&icons);
    if (!host)
        return FALSE;
    GetClientRect(host, &rc);
    e.wall = CreateWindowExW(WS_EX_NOACTIVATE, APP L"_wall", L"", WS_CHILD | WS_VISIBLE | WS_DISABLED, 0, 0,
                             rc.right, rc.bottom, host, NULL, GetModuleHandleW(NULL), NULL);
    if (!e.wall)
        return FALSE;
    e.host = host;
    e.icons = icons;
    if (icons)
        SetWindowPos(e.wall, icons, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (!gfx_attach(e.wall, rc.right, rc.bottom, want_scale())) {
        wall_detach();
        return FALSE;
    }
    log_line(L"attached to the desktop (%s layout, %ux%u)", icons ? L"24H2" : L"legacy", rc.right, rc.bottom);
    render(FALSE, TRUE); /* show a frame right away, even if we start paused */
    return TRUE;
}

static void trim_now(void)
{
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
}

/* Hands memory back to Windows. Whatever is needed again comes back on the
   next frame (from RAM, if Windows still has it, which it usually does).

   While drawing: once, 5 s after drawing starts. Most of the ~14 MB a fresh
   D3D device holds (shader compilation, driver setup) is never touched again;
   the per-frame set is ~2-3 MB, and it stays at that (FINDINGS.md).

   While idle: after 2 s, and again once a minute, since the 500 ms poll
   touches a few pages. */
static void trim_memory(void)
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (e.running) {
        if (!e.settled && now.QuadPart - e.run_since >= 5 * e.qpf) {
            trim_now();
            e.settled = TRUE;
        }
        return;
    }
    if (now.QuadPart - e.idle_since < 2 * e.qpf)
        return; /* wait a little: covering and uncovering often come in quick pairs */
    if (!e.parked && e.src && e.src->park && now.QuadPart - e.idle_since >= 10 * e.qpf) {
        e.src->park(TRUE); /* e.g. video: drop the whole decoder pipeline */
        e.parked = TRUE;
        e.trimmed = FALSE;
    }
    if (e.trimmed && now.QuadPart - e.trimmed_at < 60 * e.qpf)
        return;
    trim_now();
    e.trimmed = TRUE;
    e.trimmed_at = now.QuadPart;
}

/* Gives a still picture to Windows and drops everything graphical. */
static BOOL go_still(void)
{
    if (!still_apply(&cfg))
        return FALSE;
    wall_detach();
    gfx_free();
    e.still = TRUE;
    e.running = FALSE;
    CancelWaitableTimer(e.timer);
    e.idle_since = 0; /* trim at the next poll */
    e.trimmed = FALSE;
    update_state();
    return TRUE;
}

/* Open cfg.current, or a built-in if that fails (or fallback is set). */
static void load_source(BOOL fallback)
{
    const WCHAR *id = cfg.current;
    const Source *s = NULL;
    Kind k = kind_of(id);

    if (e.src) {
        e.src->close();
        e.parked = FALSE;
        e.src = NULL;
    }
    memset(&e.info, 0, sizeof e.info);
    e.error = FALSE;
    e.has_click = FALSE;
    source_fit = cfg.fit;
    source_speed = cfg.speed / 100.0f;

    if (!fallback && k == KIND_IMAGE && go_still())
        return;
    e.still = FALSE;
    if (!gfx.dev && !gfx_init(cfg.low_power)) {
        update_state(); /* the poll tries again */
        return;
    }

    if (!fallback && k != KIND_UNKNOWN) {
        s = source_for(k);
        if (!s->open(id, &e.info))
            s = NULL;
    }
    if (!s) {
        log_line(L"could not show %s; showing %s instead", id, g_builtins[0].name);
        e.error = TRUE;
        memset(&e.info, 0, sizeof e.info);
        s = &src_shader;
        if (!s->open(g_builtins[0].id, &e.info))
            s = NULL;
    }
    e.src = s;
    update_interval();
    /* Re-evaluate running for the new source (and start/stop its decoder). */
    e.running = FALSE;
    CancelWaitableTimer(e.timer);
    if (gfx.sc && !gfx_resize(gfx.ww, gfx.wh, want_scale()))
        wall_detach();
    render(FALSE, TRUE);
    update_state();
}

/* Rebuild the device after a GPU reset or when switching GPUs. */
static void recover(void)
{
    log_line(L"rebuilding the graphics device");
    e.lost = FALSE;
    if (e.src) {
        e.src->close();
        e.parked = FALSE;
        e.src = NULL;
    }
    wall_detach();
    gfx_free();
    e.running = FALSE;
    CancelWaitableTimer(e.timer);
    load_source(FALSE); /* the next poll attaches the window again */
}

/* At sign-in the PC is busy and Explorer is still building the desktop.
   Until the wallpaper is first on screen, the engine runs at normal priority
   and checks every STARTUP_POLL_MS; after that it goes back to its quiet
   settings: below-normal priority, efficiency mode (EcoQoS), POLL_MS. */
#define STARTUP_POLL_MS 100

static void start_quiet(BOOL attached)
{
    PROCESS_POWER_THROTTLING_STATE eco;
    e.quiet_mode = TRUE;
    log_line(attached ? L"on the desktop %u ms after start (graphics ready at %u ms, desktop found at %u ms)"
                      : L"still not on the desktop %u ms after start (graphics ready at %u ms, desktop found at %u ms)",
             GetTickCount() - e.start_tick, e.gpu_ms, e.progman_ms);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    memset(&eco, 0, sizeof eco);
    eco.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    eco.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    eco.StateMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &eco, sizeof eco);
    SetCoalescableTimer(e.msg, 1, POLL_MS, NULL, 100);
}

static void poll(void)
{
    /* SendMessageTimeout and CreateWindow dispatch incoming messages while
       they wait, which could re-enter here and attach twice. */
    static BOOL busy;
    LARGE_INTEGER now;
    RECT rc;
    if (busy)
        return;
    busy = TRUE;

    if (!gfx.dev && !e.still)
        load_source(FALSE); /* creates the device, e.g. once the GPU is ready at sign-in */
    if (e.wall && (!IsWindow(e.host) || !IsWindow(e.wall)))
        wall_detach(); /* Explorer restarted */
    if (!e.quiet_mode) { /* still starting: note when each piece became ready */
        DWORD t = GetTickCount() - e.start_tick;
        if (!e.gpu_ms && gfx.dev)
            e.gpu_ms = t ? t : 1;
        if (!e.progman_ms && FindWindowW(L"Progman", NULL))
            e.progman_ms = t ? t : 1;
    }
    if (!e.wall && gfx.dev && wall_attach() && !e.quiet_mode)
        start_quiet(TRUE);
    if (!e.quiet_mode && GetTickCount() - e.start_tick > 60000)
        start_quiet(FALSE); /* never wait at full speed for long */
    if (e.wall) {
        GetClientRect(e.host, &rc);
        if ((UINT)rc.right != gfx.ww || (UINT)rc.bottom != gfx.wh) {
            SetWindowPos(e.wall, NULL, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
            if (gfx_resize(rc.right, rc.bottom, want_scale()))
                render(FALSE, TRUE);
            else
                wall_detach();
        }
    }
    if (e.wall && e.icons && GetWindow(e.icons, GW_HWNDNEXT) != e.wall)
        SetWindowPos(e.wall, e.icons, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    e.covered = e.wall && e.info.animated && cfg.pause_covered && !e.locked && !e.display_off &&
                desktop_covered(e.msg);
    if (e.src && e.src->failed && e.src->failed()) {
        log_line(L"playback of %s failed", cfg.current);
        load_source(TRUE);
    }

    QueryPerformanceCounter(&now);
    if (now.QuadPart - e.fps_since >= e.qpf) {
        e.fps = (UINT)(e.frames * e.qpf / (now.QuadPart - e.fps_since));
        e.frames = 0;
        e.fps_since = now.QuadPart;
    }

    update_state();
    if (e.lost)
        recover();
    trim_memory();
    busy = FALSE;
}

/* Ctrl+Alt+L: see lock_now(). Another app may own the keys already. */
static void update_hotkey(void)
{
    static BOOL on;
    if (cfg.lock_hotkey == on)
        return;
    if (cfg.lock_hotkey) {
        on = RegisterHotKey(e.msg, 1, LOCK_HOTKEY_MODS | MOD_NOREPEAT, LOCK_HOTKEY_KEY);
        if (!on)
            log_line(L"Ctrl+Alt+L is already used by another app (%u)", GetLastError());
    } else {
        UnregisterHotKey(e.msg, 1);
        on = FALSE;
    }
}

static void reload(void)
{
    WCHAR prev[MAX_PATH];
    UINT scale = cfg.scale, speed = cfg.speed, bright = cfg.brightness;
    BOOL low = cfg.low_power;
    Fit fit = cfg.fit;

    str_copy(prev, cfg.current, MAX_PATH);
    config_load(&cfg);
    update_hotkey();
    source_fit = cfg.fit;
    source_speed = cfg.speed / 100.0f;
    if (e.still && str_ieq(prev, cfg.current)) {
        if (fit != cfg.fit || bright != cfg.brightness)
            load_source(FALSE); /* hand the picture over again with the new look */
    } else if (low != cfg.low_power && !e.still) {
        recover(); /* move to the other GPU */
    } else if (!str_ieq(prev, cfg.current) || (e.error && kind_of(cfg.current) != KIND_UNKNOWN)) {
        load_source(FALSE);
    } else {
        update_interval();
        if (scale != cfg.scale && gfx.sc && !gfx_resize(gfx.ww, gfx.wh, want_scale()))
            wall_detach();
        if (speed != cfg.speed && e.running && e.src && e.src->play)
            e.src->play(TRUE); /* applies the new playback rate */
        render(FALSE, TRUE);   /* fit, brightness, quality */
    }
    if (e.running)
        update_interval();
    poll();
}

static void select_wallpaper(const WCHAR *id)
{
    str_copy(cfg.current, id, MAX_PATH);
    config_save(&cfg);
    load_source(FALSE);
}

static void menu_label(WCHAR *out, const WCHAR *name, const WCHAR *suffix)
{
    int n = 0;
    for (; *name && n < MAX_PATH - 2; name++) { /* '&' is a menu accelerator: double it */
        if (*name == '&')
            out[n++] = '&';
        out[n++] = *name;
    }
    out[n] = 0;
    str_cat(out, suffix, MAX_PATH + 32);
}

static void tray_menu(void)
{
    WCHAR label[MAX_PATH + 32];
    HMENU m = CreatePopupMenu();
    UINT i, cur = 0, cmd;
    POINT p;

    for (i = 0; i < g_builtin_count; i++) {
        menu_label(label, g_builtins[i].name, g_builtins[i].interactive ? L"  \x00B7  interactive" : L"");
        AppendMenuW(m, MF_STRING, CMD_BUILTIN + i, label);
        if (str_ieq(cfg.current, g_builtins[i].id))
            cur = CMD_BUILTIN + i;
    }
    if (cfg.lib_count)
        AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    for (i = 0; i < cfg.lib_count; i++) {
        menu_label(label, path_file(cfg.lib[i]), L"");
        AppendMenuW(m, MF_STRING, CMD_LIBRARY + i, label);
        if (str_ieq(cfg.current, cfg.lib[i]))
            cur = CMD_LIBRARY + i;
    }
    if (cur)
        CheckMenuRadioItem(m, CMD_BUILTIN, CMD_LIBRARY + LIB_MAX, cur, MF_BYCOMMAND);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_ADD, L"Add wallpaper\x2026");
    AppendMenuW(m, MF_STRING, CMD_SETTINGS, L"Settings\x2026");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING | (e.user_paused ? MF_CHECKED : 0), CMD_PAUSE, L"Pause");
    AppendMenuW(m, MF_STRING | (cfg.autostart ? MF_CHECKED : 0), CMD_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, CMD_EXIT, L"Exit");
    SetMenuDefaultItem(m, CMD_SETTINGS, FALSE);

    GetCursorPos(&p);
    SetForegroundWindow(e.msg);
    cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, 0, e.msg, NULL);
    PostMessageW(e.msg, WM_NULL, 0, 0);
    DestroyMenu(m);

    if (cmd >= CMD_BUILTIN && cmd < CMD_BUILTIN + g_builtin_count) {
        select_wallpaper(g_builtins[cmd - CMD_BUILTIN].id);
    } else if (cmd >= CMD_LIBRARY && cmd < CMD_LIBRARY + cfg.lib_count) {
        WCHAR id[MAX_PATH];
        str_copy(id, cfg.lib[cmd - CMD_LIBRARY], MAX_PATH);
        select_wallpaper(id);
    } else if (cmd == CMD_PAUSE) {
        e.user_paused = !e.user_paused;
        update_state();
    } else if (cmd == CMD_AUTOSTART) {
        BOOL on = !cfg.autostart;
        if (autostart_apply(on)) {
            cfg.autostart = on;
            if (on)
                autostart_command(cfg.autostart_exe);
            config_save(&cfg);
        }
    } else if (cmd == CMD_ADD) {
        spawn_self(L"-add");
    } else if (cmd == CMD_SETTINGS) {
        spawn_self(L"-settings");
    } else if (cmd == CMD_EXIT) {
        /* Put back Windows' grace period if the settings window set it to 0
           ("Ask for PIN right away"). Re-read: the engine doesn't reload for
           lock screen settings. A separate process asks for admin rights. */
        config_load(&cfg);
        if (cfg.grace_before[0])
            spawn_self(L"-" GRACE_RESTORE_FLAG);
        DestroyWindow(e.msg);
    }
}

/* ---- window procedures ------------------------------------------------- */

static LRESULT CALLBACK wall_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(h, NULL); return 0;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_DESTROY:
        if (h == e.wall) { /* destroyed by Explorer, not by us */
            gfx_detach();
            e.wall = e.host = e.icons = NULL;
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static LRESULT CALLBACK msg_proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m && m == e.wm_reload) {
        reload();
        return 0;
    }
    if (m && m == e.wm_status) {
        return (LRESULT)e.status | ((LRESULT)(e.running ? e.fps : 0) << 8) | ((LRESULT)gfx.bw << 24) |
               ((LRESULT)gfx.bh << 40);
    }
    if (m && m == e.wm_pause) {
        e.user_paused = w == 2 ? !e.user_paused : w != 0;
        update_state();
        return 0;
    }
    if (m && m == e.wm_taskbar) { /* Explorer restarted */
        tray(NIM_ADD);
        poll();
        return 0;
    }
    switch (m) {
    case WM_HOTKEY:
        if (w == 1 && lock_now()) {
            e.lock_waited = 0;
            SetTimer(h, 2, 100, NULL);
        }
        return 0;
    case WM_TIMER:
        if (w == 2) {
            e.lock_waited += 100;
            if (lock_check(e.lock_waited))
                KillTimer(h, 2);
            return 0;
        }
        poll();
        return 0;
    case WM_DISPLAYCHANGE:
        poll();
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (w == WTS_SESSION_LOCK) {
            e.locked = TRUE;
            lock_note_locked();
        }
        if (w == WTS_SESSION_UNLOCK) e.locked = FALSE;
        if (w == WTS_SESSION_LOCK || w == WTS_SESSION_UNLOCK)
            log_line(w == WTS_SESSION_LOCK ? L"PC locked" : L"PC unlocked");
        poll();
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_POWERSETTINGCHANGE) {
            POWERBROADCAST_SETTING *s = (POWERBROADCAST_SETTING *)l;
            DWORD v = *(DWORD *)s->Data;
            if (!memcmp(&s->PowerSetting, &kDisplayState, sizeof(GUID))) e.display_off = v == 0;
            if (!memcmp(&s->PowerSetting, &kPowerSource, sizeof(GUID))) e.on_battery = v != 0;
            poll();
        }
        return TRUE;
    case WM_TRAY:
        if (LOWORD(l) == WM_RBUTTONUP || LOWORD(l) == WM_CONTEXTMENU)
            tray_menu();
        else if (LOWORD(l) == WM_LBUTTONUP)
            spawn_self(L"-settings");
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ---- entry ------------------------------------------------------------- */

/* quiet: started by the sign-in task (-startup): never open a window. */
void engine_run(BOOL quiet)
{
    HINSTANCE inst = GetModuleHandleW(NULL);
    HPOWERNOTIFY pn1, pn2;
    WCHAR cmd[MAX_PATH + 16];
    LARGE_INTEGER f;
    WNDCLASSW wc;
    HANDLE mutex;
    BOOL first;
    MSG m;

    mutex = CreateMutexW(NULL, TRUE, L"Local\\livewall.engine");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        /* Already running. Opened again by the user: show settings.
           Started again at sign-in: nothing to do. */
        if (mutex)
            CloseHandle(mutex);
        if (!quiet)
            settings_run(SETTINGS_MAIN);
        return;
    }
    e.start_tick = GetTickCount();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    allow_dark_menus();
    /* Below-normal priority and EcoQoS come once the wallpaper is on the
       desktop (start_quiet), so they don't slow down the start. */

    first = !config_load(&cfg);
    autostart_command(cmd);
    /* First run, the exe moved, or a task from before -startup existed. */
    if (cfg.autostart && !str_ieq(cfg.autostart_exe, cmd) && autostart_apply(TRUE))
        str_copy(cfg.autostart_exe, cmd, MAX_PATH + 16);
    if (first || !str_ieq(cfg.autostart_exe, cmd))
        config_save(&cfg);
    lock_refresh(); /* the screen saver copy follows this exe after an update */

    QueryPerformanceFrequency(&f);
    e.qpf = f.QuadPart;
    e.fps_since = 0;
    /* High-resolution timer without raising the system-wide timer rate. */
    e.timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!e.timer)
        e.timer = CreateWaitableTimerW(NULL, FALSE, NULL);

    memset(&wc, 0, sizeof wc);
    wc.hInstance = inst;
    wc.lpfnWndProc = wall_proc;
    wc.lpszClassName = APP L"_wall";
    RegisterClassW(&wc);
    wc.lpfnWndProc = msg_proc;
    wc.lpszClassName = ENGINE_CLASS;
    RegisterClassW(&wc);
    e.wm_taskbar = RegisterWindowMessageW(L"TaskbarCreated");
    e.wm_reload = RegisterWindowMessageW(MSG_RELOAD);
    e.wm_status = RegisterWindowMessageW(MSG_STATUS);
    e.wm_pause = RegisterWindowMessageW(MSG_PAUSE);
    e.msg = CreateWindowExW(WS_EX_TOOLWINDOW, ENGINE_CLASS, APP, WS_POPUP, 0, 0, 0, 0, NULL, NULL, inst, NULL);
    update_hotkey();

    e.icon = make_app_icon(GetSystemMetrics(SM_CXSMICON));
    e.status = ST_WAITING;
    tray(NIM_ADD);
    log_line(L"engine started: %s", cfg.current);

    load_source(FALSE); /* a still picture never even creates a graphics device */
    if (!gfx.dev && !e.still)
        log_line(L"Direct3D 11 is not available yet; will retry");

    WTSRegisterSessionNotification(e.msg, NOTIFY_FOR_THIS_SESSION);
    pn1 = RegisterPowerSettingNotification(e.msg, &kDisplayState, DEVICE_NOTIFY_WINDOW_HANDLE);
    pn2 = RegisterPowerSettingNotification(e.msg, &kPowerSource, DEVICE_NOTIFY_WINDOW_HANDLE);
    SetCoalescableTimer(e.msg, 1, STARTUP_POLL_MS, NULL, 0);
    poll();
    if (e.still && !e.quiet_mode)
        start_quiet(TRUE); /* a still picture never attaches a window */
    if (first && !quiet)
        spawn_self(L"-settings"); /* first launch by the user: show what it is */

    /* Sleep until the frame timer fires or a message arrives. */
    for (;;) {
        DWORD n = e.running ? 1 : 0;
        DWORD r = MsgWaitForMultipleObjectsEx(n, &e.timer, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (n && r == WAIT_OBJECT_0 && e.running) {
            LARGE_INTEGER now;
            render(TRUE, FALSE);
            if (e.lost) {
                recover();
            } else {
                QueryPerformanceCounter(&now);
                e.due += e.interval;
                if (e.due < now.QuadPart)
                    e.due = now.QuadPart + e.interval; /* fell behind: don't catch up */
                arm_timer(e.due);
            }
        }
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            if (m.message == WM_QUIT)
                goto done;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }

done:
    log_line(L"engine stopped");
    UnregisterPowerSettingNotification(pn1);
    UnregisterPowerSettingNotification(pn2);
    WTSUnRegisterSessionNotification(e.msg);
    tray(NIM_DELETE);
    if (e.src)
        e.src->close();
    wall_detach();
    gfx_free();
    if (e.icon)
        DestroyIcon(e.icon);
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
}
