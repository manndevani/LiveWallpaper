/* main.c - livewall entry point. One small executable, three roles:
 *
 *   livewall.exe                 the wallpaper engine (tray icon, always running)
 *   livewall.exe -settings       the settings window (separate, short-lived)
 *   livewall.exe -add            settings window, straight to "add files"
 *   livewall.exe -quit           stop the running engine
 *   livewall.exe -snapshot <id> <out.png> [seconds]   render one frame to a PNG
 *
 * Plain C, no C runtime: see util.c. */
#include "common.h"
#include "ipc.h"
#include "engine.h"
#include "settings.h"
#include "preview.h"

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
        settings_run(FALSE);
    } else if (is_flag(arg, L"add")) {
        settings_run(TRUE);
    } else if (is_flag(arg, L"snapshot")) {
        rest = next_arg(rest, id, MAX_PATH);
        rest = next_arg(rest, out, MAX_PATH);
        next_arg(rest, secs, 32);
        code = snapshot_run(id, out, secs[0] ? parse_seconds(secs) : 8.0);
    } else {
        engine_run();
    }
    ExitProcess((UINT)code);
}
