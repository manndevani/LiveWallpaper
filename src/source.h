/* source.h - a "source" is anything that can draw the wallpaper: a shader,
 * a video, or an image/GIF. The engine owns timing and pausing; a source only
 * opens a file, draws when asked, and closes. One source is open at a time. */
#pragma once
#include "gfx.h"

typedef struct {
    BOOL animated;    /* FALSE: draw once, no frame timer (still images) */
    BOOL full_res;    /* ignore the shader-quality setting (images, video) */
    BOOL interactive; /* wants the cursor position and clicks */
    UINT tick_fps;    /* wake-up rate override, 0 = use the fps setting */
} SourceInfo;

typedef struct Source {
    BOOL (*open)(const WCHAR *id, SourceInfo *info);
    void (*close)(void);
    /* Draw into gfx.rtv / gfx.backbuf. Returns TRUE when a new frame was
       drawn and needs presenting. force: draw even if nothing changed. */
    BOOL (*draw)(const FrameInfo *f, BOOL force);
    void (*play)(BOOL on); /* optional: start/stop background decoding */
    BOOL (*failed)(void);  /* optional: playback broke after opening */
    /* optional: after a long pause, free the decoder (TRUE) and bring it
       back where it stopped (FALSE). The last frame stays on screen. */
    void (*park)(BOOL park);
} Source;

extern const Source src_shader, src_video, src_image;

static __inline const Source *source_for(Kind k)
{
    if (k == KIND_VIDEO)
        return &src_video;
    if (k == KIND_GIF || k == KIND_IMAGE)
        return &src_image;
    return &src_shader;
}
