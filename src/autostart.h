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

#ifdef __cplusplus
}
#endif
