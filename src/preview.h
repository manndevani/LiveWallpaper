/* preview.h - render any wallpaper into an offscreen image. Used for the
 * settings window thumbnails and for `livewall.exe -snapshot`. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

BOOL preview_begin(void); /* creates a GPU device if this process has none */
void preview_end(void);
/* Renders id at time `at` (seconds) into bgra (w*h*4, top-down). */
BOOL preview_render(const WCHAR *id, UINT w, UINT h, double at, BYTE *bgra);
BOOL preview_save_png(const WCHAR *path, UINT w, UINT h, const BYTE *bgra);
/* livewall.exe -snapshot <id or file> <out.png> [seconds]: returns exit code */
int snapshot_run(const WCHAR *id, const WCHAR *out, double at);
int check_run(const WCHAR *file, const WCHAR *out); /* -check: see preview.c */
/* Why a custom .hlsl can't be shown ("line 12: undeclared identifier 'foo'"),
   or FALSE if it compiles. In src_shader.c. Loads the compiler for the call. */
BOOL shader_problem(const WCHAR *path, WCHAR *out, int max);

#ifdef __cplusplus
}
#endif
