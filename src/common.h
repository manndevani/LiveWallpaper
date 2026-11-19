/* common.h - shared by every module (C and C++). */
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <windows.h>

#define APP L"livewall"
#define ENGINE_CLASS APP L"_engine"     /* hidden window of the wallpaper engine */
#define SETTINGS_CLASS APP L"_settings" /* the settings window */
#define MSG_RELOAD L"livewall.reload"   /* registered message: config.ini changed */
#define MSG_STATUS L"livewall.status"   /* registered message: returns packed status */
#define MSG_PAUSE L"livewall.pause"     /* registered message: wParam 0 resume, 1 pause, 2 toggle */

#ifdef __cplusplus
#define RELEASE(p) do { if (p) { (p)->Release(); (p) = NULL; } } while (0)
extern "C" {
#else
#define RELEASE(p) do { if (p) { (p)->lpVtbl->Release(p); (p) = NULL; } } while (0)
#endif

typedef enum {
    ST_NOT_RUNNING = 0, /* also what a failed query returns */
    ST_RUNNING,
    ST_WAITING,         /* desktop not ready yet (early in boot, Explorer restarting) */
    ST_PAUSED,          /* paused by the user */
    ST_LOCKED,
    ST_DISPLAY_OFF,
    ST_BATTERY,
    ST_COVERED,
    ST_ERROR,           /* the chosen file failed; a built-in is shown instead */
    ST_STILL,           /* a still image: drawn once, nothing to animate */
} EngineStatus;

void *mem_alloc(SIZE_T n); /* zeroed */
void mem_free(void *p);

int str_len(const WCHAR *s);
void str_copy(WCHAR *dst, const WCHAR *src, int cap);
void str_cat(WCHAR *dst, const WCHAR *src, int cap);
BOOL str_ieq(const WCHAR *a, const WCHAR *b);
BOOL str_starts(const WCHAR *s, const WCHAR *prefix);
const WCHAR *path_file(const WCHAR *path);
const WCHAR *path_ext(const WCHAR *path); /* includes the dot, "" if none */

void app_dir(WCHAR *out); /* %APPDATA%\livewall, created if missing */
void log_line(const WCHAR *fmt, ...);
void log_text(const char *text, int len);

HICON make_app_icon(int size);
void allow_dark_menus(void); /* popup menus follow the system dark mode */

#ifdef __cplusplus
}
#endif
