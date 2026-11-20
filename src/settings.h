/* settings.h - the settings window (livewall.exe -settings). */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { SETTINGS_MAIN, SETTINGS_ADD, SETTINGS_LOCK }; /* what to show first */

void settings_run(int page);
void grace_restore_run(void); /* -put-back-grace: see settings.cpp */
#define GRACE_RESTORE_FLAG L"put-back-grace"

#ifdef __cplusplus
}
#endif
