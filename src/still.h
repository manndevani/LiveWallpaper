/* still.h - still pictures are handed to Windows' own wallpaper, so the
 * engine can drop its graphics device and use almost no memory. */
#pragma once
#include "common.h"
#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sets c->current (an image file) as the Windows desktop wallpaper, with
   c->fit and c->brightness applied. Returns FALSE if Windows refused it. */
BOOL still_apply(const Config *c);

#ifdef __cplusplus
}
#endif
