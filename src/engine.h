/* engine.h - the always-running wallpaper process. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

void engine_run(BOOL quiet); /* quiet: started at sign-in, never open a window */

#ifdef __cplusplus
}
#endif
