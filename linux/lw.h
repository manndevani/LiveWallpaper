/* lw.h - livewall for Linux: shared declarations.
 *
 * Same idea as the Windows engine: one small program that draws a shader,
 * video, GIF or picture behind the desktop, pauses when nobody can see it,
 * and reads ~/.config/livewall/config.ini (the single source of truth).
 *
 * Backends: Wayland (wlr-layer-shell "background" layer) and X11 (a
 * _NET_WM_WINDOW_TYPE_DESKTOP window). Drawing: EGL + OpenGL ES 2.0.
 * Video/GIF/pictures: libmpv, loaded at runtime (optional). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#ifndef LW_VERSION
#define LW_VERSION "dev" /* the Makefile passes the version from src/version.h */
#endif
#define PERIOD_S 120.0 /* built-in shaders loop seamlessly every PERIOD_S seconds */
#define LIB_MAX 64
#define PATH_LEN 1024

typedef enum { KIND_SHADER, KIND_CUSTOM_SHADER, KIND_MEDIA, KIND_UNKNOWN } Kind;
typedef enum { FIT_COVER, FIT_CONTAIN, FIT_STRETCH } Fit;

typedef struct {
    char current[PATH_LEN]; /* "builtin:<name>" or a file path */
    int fps;                /* frame cap while visible */
    int resolution;         /* shaders: 0 auto, 1 full, 2 half */
    int speed;              /* animation speed, percent */
    int brightness;         /* percent */
    Fit fit;
    bool pause_covered, pause_battery, autostart;
    int lib_count;
    char lib[LIB_MAX][PATH_LEN];
} Config;

typedef struct {
    const char *id, *name, *src;
    bool interactive;
} Builtin;

/* config.c */
extern const Builtin g_builtins[];
extern const int g_builtin_count;
const char *config_dir(void);           /* ~/.config/livewall (created) */
bool config_load(Config *c);            /* false if there was no file yet */
void config_save(const Config *c);
Kind kind_of(const char *id);
const Builtin *builtin_find(const char *id);
bool lib_add(Config *c, const char *path);
void lw_log(const char *fmt, ...);

/* What a frame needs to know. Mouse and click are in pixels, y down. */
typedef struct {
    double time;
    float phase;
    float mouse[2], click[2];
    float click_age, mouse_on;
} Frame;
float phase_at(double t);

/* gl.c - shaders and the fullscreen pass, on whatever context is current. */
typedef struct Shader Shader;
Shader *shader_load(const char *id);    /* builtin:<name> or a .glsl/.frag file */
void shader_draw(Shader *s, int w, int h, const Frame *f, int scale);
void shader_free(Shader *s);
void dim_draw(float brightness);        /* darkens what is already drawn */

/* media.c - libmpv. All functions are safe to call when libmpv is missing. */
bool media_available(void);
bool media_open(const char *path, Fit fit, int wake_fd);
void media_draw(int fbo, int w, int h, bool flip_y);
bool media_has_frame(void);             /* consume the "new frame" flag */
void media_pause(bool pause);
void media_seek(double sec);
void media_close(void);
void media_service(void);               /* call when wake_fd is readable */

/* png.c */
bool png_write(const char *path, const uint8_t *rgba, int w, int h);

/* Window-system backends (wayland.c / x11.c). */
typedef struct {
    bool (*init)(void);
    int  (*fd)(void);                  /* poll() this */
    void (*dispatch)(bool readable);   /* readable: fd had data; false: just flush before polling */
    int  (*outputs)(void);             /* number of surfaces to draw */
    bool (*begin)(int i, int *w, int *h); /* make surface i current; false = skip it now */
    void (*end)(int i);                 /* swap */
    bool (*covered)(void);             /* a fullscreen/maximized window hides the desktop */
    bool (*waiting)(void);             /* every surface is waiting for the compositor (hidden) */
    void (*pointer)(int i, Frame *f, double now); /* fill mouse/click for surface i */
    void (*shutdown)(void);
} Backend;
extern const Backend wayland_backend, x11_backend;
extern EGLDisplay g_egl_dpy;
extern EGLContext g_egl_ctx;
extern EGLConfig  g_egl_cfg;
bool egl_setup(EGLenum platform, void *native_display);
