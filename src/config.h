/* config.h - settings and the wallpaper library, stored in
 * %APPDATA%\livewall\config.ini. The single source of truth shared by the
 * engine and the settings window. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIB_MAX 64

typedef enum { KIND_SHADER, KIND_CUSTOM_SHADER, KIND_VIDEO, KIND_GIF, KIND_IMAGE, KIND_UNKNOWN } Kind;
typedef enum { FIT_COVER, FIT_CONTAIN, FIT_STRETCH } Fit;

typedef struct {
    const WCHAR *id, *name;
    BOOL interactive;
} Builtin;

extern const Builtin g_builtins[];
extern const UINT g_builtin_count;

typedef struct {
    WCHAR current[MAX_PATH]; /* "builtin:<name>" or a file path */
    UINT fps;                /* frame cap while visible */
    UINT scale;              /* shaders render at 1/scale resolution; 0 = auto from the display */
    UINT speed;              /* animation speed, percent */
    UINT brightness;         /* percent */
    Fit fit;                 /* images and video */
    BOOL pause_covered, pause_battery, low_power, autostart;
    WCHAR autostart_exe[MAX_PATH + 16]; /* what the startup task runs: "<exe> -startup" */
    WCHAR lock[MAX_PATH];    /* lock screen wallpaper; empty = same as the desktop */
    BOOL lock_picture;       /* also set the Windows lock screen picture to match */
    BOOL lock_hotkey;        /* Ctrl+Alt+L starts the screen saver (and so the lock) */
    UINT lib_count;
    WCHAR lib[LIB_MAX][MAX_PATH]; /* files the user added */
} Config;

BOOL config_load(Config *c); /* FALSE if there was no file yet (defaults filled in) */
void config_save(const Config *c);

Kind kind_of(const WCHAR *id);
const WCHAR *kind_label(Kind k);
const Builtin *builtin_find(const WCHAR *id);
const WCHAR *display_name(const WCHAR *id);
BOOL lib_add(Config *c, const WCHAR *path);
void lib_remove(Config *c, UINT index);

#ifdef __cplusplus
}
#endif
