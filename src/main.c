/* main.c - livewall entry point. One small executable, three roles:
 *
 *   livewall.exe                 the wallpaper engine (tray icon, always running)
 *   livewall.exe -startup        the engine, started at sign-in: never opens a window
 *   livewall.exe -settings       the settings window (separate, short-lived)
 *   livewall.exe -add            settings window, straight to "add files"
 *   livewall.exe -quit           stop the running engine
 *   livewall.exe -put-back-grace put back Windows' grace period (engine, on Exit)
 *   livewall.exe -snapshot <id> <out.png> [seconds]   render one frame to a PNG
 *   livewall.exe -check <file> <out.png>   the gallery's safety check (preview.c)
 *   livewall.scr /s, /p <hwnd>, /c   the screen saver (see saver.c)
 *
 * Plain C, no C runtime: see util.c. */
#include "common.h"
#include "ipc.h"
#include "engine.h"
#include "settings.h"
#include "preview.h"
#include "autostart.h"
#include "lock.h"

/* Copies the next (optionally quoted) argument into out; returns the rest. */
static const WCHAR *next_arg(const WCHAR *s, WCHAR *out, int cap)
{
    int n = 0;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '"') {
        for (s++; *s && *s != '"'; s++)
            if (n < cap - 1)
                out[n++] = *s;
        if (*s)
            s++;
    } else {
        for (; *s && *s != ' ' && *s != '\t'; s++)
            if (n < cap - 1)
                out[n++] = *s;
    }
    out[n] = 0;
    return s;
}

static BOOL is_flag(const WCHAR *arg, const WCHAR *name)
{
    if (*arg != '-' && *arg != '/')
        return FALSE;
    return str_ieq(arg + 1, name);
}

/* Screen saver arguments: /s, /p <hwnd>, /p:<hwnd>, /c, /c:<hwnd>. */
static BOOL is_saver_flag(const WCHAR *arg)
{
    WCHAR c = arg[1] | 0x20;
    return (arg[0] == '/' || arg[0] == '-') && (c == 's' || c == 'p' || c == 'c') && (arg[2] == 0 || arg[2] == ':');
}

/* Started as livewall.scr: Windows runs a screen saver with no arguments to
   configure it. */
static BOOL started_as_scr(void)
{
    WCHAR exe[MAX_PATH];
    return GetModuleFileNameW(NULL, exe, MAX_PATH) && str_ieq(path_ext(exe), L".scr");
}

static double parse_seconds(const WCHAR *s)
{
    double v = 0, scale = 0;
    for (; *s; s++) {
        if (*s >= '0' && *s <= '9') {
            if (scale) {
                v += (*s - '0') * scale;
                scale /= 10;
            } else {
                v = v * 10 + (*s - '0');
            }
        } else if (*s == '.') {
            scale = 0.1;
        }
    }
    return v;
}

void __stdcall entry(void)
{
    static WCHAR arg[MAX_PATH], id[MAX_PATH], out[MAX_PATH], secs[32];
    const WCHAR *rest = next_arg(GetCommandLineW(), arg, MAX_PATH); /* skip the exe */
    int code = 0;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    rest = next_arg(rest, arg, MAX_PATH);

    if (is_flag(arg, L"quit")) {
        engine_stop();
    } else if (is_flag(arg, L"settings")) {
        settings_run(SETTINGS_MAIN);
    } else if (is_flag(arg, L"add")) {
        settings_run(SETTINGS_ADD);
    } else if (is_flag(arg, L"check")) {
        rest = next_arg(rest, id, MAX_PATH);
        next_arg(rest, out, MAX_PATH);
        code = check_run(id, out);
    } else if (is_flag(arg, GRACE_RESTORE_FLAG)) {
        grace_restore_run();
    } else if (is_flag(arg, L"snapshot")) {
        rest = next_arg(rest, id, MAX_PATH);
        rest = next_arg(rest, out, MAX_PATH);
        next_arg(rest, secs, 32);
        code = snapshot_run(id, out, secs[0] ? parse_seconds(secs) : 8.0);
    } else if (is_saver_flag(arg) || (!arg[0] && started_as_scr())) {
        code = saver_run(arg, rest);
    } else {
        engine_run(is_flag(arg, AUTOSTART_FLAG));
    }
    ExitProcess((UINT)code);
}
