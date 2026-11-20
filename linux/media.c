/* media.c - videos, GIFs and pictures through libmpv's render API.
 *
 * libmpv is loaded with dlopen() when first needed, so livewall runs (with
 * shaders only) on systems without it. mpv decodes on the GPU where it can
 * (hwdec=auto-safe), loops forever, has no audio, and keeps only a small
 * buffer. */
#include "lw.h"
#include <mpv/client.h>
#include <mpv/render_gl.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static struct {
    void *lib;
    bool tried;
    mpv_handle *(*create)(void);
    int (*initialize)(mpv_handle *);
    void (*terminate_destroy)(mpv_handle *);
    int (*set_option_string)(mpv_handle *, const char *, const char *);
    int (*set_property_string)(mpv_handle *, const char *, const char *);
    int (*command)(mpv_handle *, const char **);
    mpv_event *(*wait_event)(mpv_handle *, double);
    void (*set_wakeup_callback)(mpv_handle *, void (*)(void *), void *);
    int (*render_create)(mpv_render_context **, mpv_handle *, mpv_render_param *);
    void (*render_set_update)(mpv_render_context *, mpv_render_update_fn, void *);
    uint64_t (*render_update)(mpv_render_context *);
    int (*render_render)(mpv_render_context *, mpv_render_param *);
    void (*render_free)(mpv_render_context *);
} M;

static mpv_handle *mpv;
static mpv_render_context *rc;
static int wake;
static bool new_frame;

static bool load(void)
{
    if (M.tried) return M.lib != NULL;
    M.tried = true;
    const char *names[] = {"libmpv.so.2", "libmpv.so.1", "libmpv.so"};
    for (size_t i = 0; i < 3 && !M.lib; i++) M.lib = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!M.lib) { lw_log("libmpv not found: videos, GIFs and pictures need the libmpv package"); return false; }
#define SYM(field, name) if (!(*(void **)&M.field = dlsym(M.lib, name))) { lw_log("libmpv: missing %s", name); dlclose(M.lib); M.lib = NULL; return false; }
    SYM(create, "mpv_create")
    SYM(initialize, "mpv_initialize")
    SYM(terminate_destroy, "mpv_terminate_destroy")
    SYM(set_option_string, "mpv_set_option_string")
    SYM(set_property_string, "mpv_set_property_string")
    SYM(command, "mpv_command")
    SYM(wait_event, "mpv_wait_event")
    SYM(set_wakeup_callback, "mpv_set_wakeup_callback")
    SYM(render_create, "mpv_render_context_create")
    SYM(render_set_update, "mpv_render_context_set_update_callback")
    SYM(render_update, "mpv_render_context_update")
    SYM(render_render, "mpv_render_context_render")
    SYM(render_free, "mpv_render_context_free")
#undef SYM
    return true;
}

bool media_available(void) { return load(); }

static void poke(void *ctx)
{
    (void)ctx;
    if (wake > 0) {
        uint64_t one = 1;
        ssize_t r = write(wake, &one, sizeof one);
        (void)r;
    }
}

static void *gl_proc(void *ctx, const char *name)
{
    (void)ctx;
    return (void *)eglGetProcAddress(name);
}

bool media_open(const char *path, Fit fit, int wake_fd)
{
    media_close();
    if (!load()) return false;
    wake = wake_fd;
    mpv = M.create();
    if (!mpv) return false;
    static const char *opts[][2] = {
        {"vo", "libmpv"}, {"profile", "fast"}, {"hwdec", "auto-safe"}, {"loop-file", "inf"}, {"aid", "no"},
        {"audio", "no"}, {"mute", "yes"}, {"image-display-duration", "inf"}, {"keep-open", "yes"},
        {"osc", "no"}, {"terminal", "no"}, {"input-default-bindings", "no"}, {"config", "no"},
        {"demuxer-max-bytes", "4MiB"}, {"demuxer-max-back-bytes", "1MiB"}, {"sub", "no"},
    };
    for (size_t i = 0; i < sizeof opts / sizeof *opts; i++) M.set_option_string(mpv, opts[i][0], opts[i][1]);
    M.set_option_string(mpv, "keepaspect", fit == FIT_STRETCH ? "no" : "yes");
    M.set_option_string(mpv, "panscan", fit == FIT_COVER ? "1.0" : "0.0");
    if (M.initialize(mpv) < 0) { M.terminate_destroy(mpv); mpv = NULL; return false; }

    mpv_opengl_init_params gl = {.get_proc_address = gl_proc};
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_OPENGL},
        {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    if (M.render_create(&rc, mpv, params) < 0) {
        lw_log("libmpv: can't create the OpenGL renderer");
        M.terminate_destroy(mpv);
        mpv = NULL;
        rc = NULL;
        return false;
    }
    M.set_wakeup_callback(mpv, poke, NULL);
    M.render_set_update(rc, poke, NULL);
    const char *cmd[] = {"loadfile", path, NULL};
    M.command(mpv, cmd);
    lw_log("playing %s", path);
    return true;
}

void media_service(void)
{
    if (!mpv) return;
    for (;;) {
        mpv_event *e = M.wait_event(mpv, 0);
        if (e->event_id == MPV_EVENT_NONE) break;
        if (e->event_id == MPV_EVENT_END_FILE) {
            mpv_event_end_file *ef = e->data;
            if (ef && ef->reason == MPV_END_FILE_REASON_ERROR) lw_log("libmpv: can't play this file");
        }
    }
    if (rc && (M.render_update(rc) & MPV_RENDER_UPDATE_FRAME)) new_frame = true;
}

bool media_has_frame(void)
{
    bool f = new_frame;
    new_frame = false;
    return f;
}

void media_draw(int fbo, int w, int h, bool flip_y)
{
    if (!rc) {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }
    mpv_opengl_fbo target = {.fbo = fbo, .w = w, .h = h};
    int flip = flip_y ? 1 : 0;
    mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &target},
        {MPV_RENDER_PARAM_FLIP_Y, &flip},
        {MPV_RENDER_PARAM_INVALID, NULL},
    };
    M.render_render(rc, params);
}

void media_pause(bool pause)
{
    if (mpv) M.set_property_string(mpv, "pause", pause ? "yes" : "no");
}

void media_seek(double sec)
{
    if (!mpv) return;
    char s[32];
    snprintf(s, sizeof s, "%.3f", sec);
    const char *cmd[] = {"seek", s, "absolute", NULL};
    M.command(mpv, cmd);
}

void media_close(void)
{
    if (rc) { M.render_free(rc); rc = NULL; }
    if (mpv) { M.terminate_destroy(mpv); mpv = NULL; }
    new_frame = false;
}
