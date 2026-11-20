/* autostart.h - "Start with Windows" via a per-user Task Scheduler logon task.
 * A logon task starts as soon as the user signs in, ahead of the Run-key
 * startup apps that Explorer staggers, and needs no admin rights. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL autostart_apply(BOOL on); /* create or delete the task */
BOOL autostart_query(void);    /* does the task exist? */

/* What the task runs: "<this exe> -startup". -startup starts the engine
   quietly (never a window). Stored as Config.autostart_exe, so an install
   whose task predates the flag, or whose exe moved, re-registers it once. */
void autostart_command(WCHAR *out); /* MAX_PATH + 16 chars */
#define AUTOSTART_FLAG L"startup"

#ifdef __cplusplus
}
#endif
