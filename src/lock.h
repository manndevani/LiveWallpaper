/* lock.h - livewall as the Windows screen saver, so the wallpaper also
 * shows when the PC sits idle, and Windows' own lock screen (with its own
 * password check) follows when it ends.
 *
 * livewall never checks passwords itself. "Ask for password" is Windows'
 * ScreenSaverIsSecure setting: when the screen saver closes, Windows locks
 * the PC and its normal sign-in screen asks for the password.
 *
 * The screen saver settings belong to Windows (Control Panel\Desktop), so
 * they are read from and written to Windows, not config.ini. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    BOOL ours;      /* livewall is the chosen screen saver and it is on */
    BOOL other;     /* another screen saver is on */
    BOOL secure;    /* Windows locks the PC when the screen saver ends */
    BOOL managed;   /* set by the organization's policy: read-only */
    UINT minutes;   /* idle time before it starts */
} LockState;

void lock_query(LockState *s);
/* Turns livewall's screen saver on or off. Returns FALSE if Windows refused. */
BOOL lock_apply(BOOL on, UINT minutes, BOOL secure);
/* Keeps the screen saver copy (%LOCALAPPDATA%\livewall\livewall.scr) the same
   as this exe after an update. Cheap: compares size and date. */
void lock_refresh(void);

/* The lock shortcut (Ctrl+Alt+L): starts the screen saver now. If it is not
   livewall's, or "Ask for password" is off, locks the PC directly instead.
   Returns TRUE when it started the screen saver: then call lock_check()
   every 100 ms until it returns TRUE. */
BOOL lock_now(void);
BOOL lock_check(UINT waited_ms);
#define SAVER_CLASS APP L"_saver" /* the screen saver's window */
#define LOCK_HOTKEY_MODS (MOD_CONTROL | MOD_ALT)
#define LOCK_HOTKEY_KEY 'L'

/* livewall.exe /s, /p <hwnd>, /c: run as the screen saver. */
int saver_run(const WCHAR *arg, const WCHAR *rest);

#ifdef __cplusplus
}
#endif
