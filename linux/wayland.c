/* wayland.c - one background-layer surface per monitor (wlr-layer-shell).
 *
 * Works on compositors that support the layer-shell protocol: Sway,
 * Hyprland, KDE Plasma, river, Wayfire, niri, labwc and others. GNOME does
 * not support it.
 *
 * Pausing comes for free: after each frame we ask the compositor for a
 * "frame done" callback and don't draw again until it arrives. A compositor
 * doesn't send it while the background is hidden (fullscreen window, screen
 * off, other workspace), so nothing is drawn and nothing is wasted. */
#include "lw.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <wayland-client.h>
#include <wayland-egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_OUT 8

typedef struct {
    struct wl_output *output;
    uint32_t name;
    int scale;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    struct wl_egl_window *egl_win;
    EGLSurface egl_surf;
    struct wl_callback *frame_cb;
    int w, h;          /* surface size, logical pixels */
    bool configured;
} Out;

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct zwlr_layer_shell_v1 *shell;
static struct wl_seat *seat;
static struct wl_pointer *ptr;
static Out out[MAX_OUT];
static int n_out;

static int ptr_out = -1;        /* surface the pointer is on */
static float ptr_x, ptr_y;      /* surface-local, logical pixels */
static float click_x, click_y;
static int click_out = -1;
static double click_at = -1e9, now_s;

/* ---- outputs ---------------------------------------------------------- */

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t sub, const char *make, const char *model, int32_t tr)
{ (void)d; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)sub; (void)make; (void)model; (void)tr; }
static void out_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r)
{ (void)d; (void)o; (void)f; (void)w; (void)h; (void)r; }
static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }
static void out_scale(void *d, struct wl_output *o, int32_t s) { (void)o; ((Out *)d)->scale = s > 0 ? s : 1; }
static void out_name(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static void out_desc(void *d, struct wl_output *o, const char *n) { (void)d; (void)o; (void)n; }
static const struct wl_output_listener out_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_desc,
};

/* ---- layer surfaces ---------------------------------------------------- */

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h)
{
    Out *o = d;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    o->w = (int)w;
    o->h = (int)h;
    int bw = o->w * o->scale, bh = o->h * o->scale;
    wl_surface_set_buffer_scale(o->surface, o->scale);
    if (!o->egl_win) {
        o->egl_win = wl_egl_window_create(o->surface, bw, bh);
        o->egl_surf = eglCreateWindowSurface(g_egl_dpy, g_egl_cfg, (EGLNativeWindowType)o->egl_win, NULL);
    } else {
        wl_egl_window_resize(o->egl_win, bw, bh, 0, 0);
    }
    o->configured = true;
}

static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l)
{
    Out *o = d;
    (void)l;
    o->configured = false; /* the monitor went away */
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {layer_configure, layer_closed};

static void make_surface(Out *o)
{
    if (o->surface || !comp || !shell) return;
    o->surface = wl_compositor_create_surface(comp);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(shell, o->surface, o->output,
                                                     ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "livewall");
    zwlr_layer_surface_v1_set_size(o->layer, 0, 0);
    zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    wl_surface_commit(o->surface);
}

static void drop_surface(Out *o)
{
    if (o->frame_cb) wl_callback_destroy(o->frame_cb);
    if (o->egl_surf) eglDestroySurface(g_egl_dpy, o->egl_surf);
    if (o->egl_win) wl_egl_window_destroy(o->egl_win);
    if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
    if (o->surface) wl_surface_destroy(o->surface);
    if (o->output) wl_output_destroy(o->output);
    memset(o, 0, sizeof *o);
}

/* ---- pointer ----------------------------------------------------------- */

static int out_of(struct wl_surface *s)
{
    for (int i = 0; i < n_out; i++) if (out[i].surface == s) return i;
    return -1;
}

static void p_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)serial;
    ptr_out = out_of(s);
    ptr_x = (float)wl_fixed_to_double(x);
    ptr_y = (float)wl_fixed_to_double(y);
}
static void p_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{ (void)d; (void)p; (void)serial; (void)s; ptr_out = -1; }
static void p_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)d; (void)p; (void)t;
    ptr_x = (float)wl_fixed_to_double(x);
    ptr_y = (float)wl_fixed_to_double(y);
}
static void p_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)t; (void)button;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED && ptr_out >= 0) {
        click_out = ptr_out;
        click_x = ptr_x;
        click_y = ptr_y;
        click_at = now_s;
    }
}
static void p_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{ (void)d; (void)p; (void)t; (void)a; (void)v; }
static void p_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void p_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void p_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d; (void)p; (void)t; (void)a; }
static void p_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d; (void)p; (void)a; (void)v; }
static const struct wl_pointer_listener ptr_listener = {
    .enter = p_enter, .leave = p_leave, .motion = p_motion, .button = p_button, .axis = p_axis,
    .frame = p_frame, .axis_source = p_axis_source, .axis_stop = p_axis_stop, .axis_discrete = p_axis_discrete,
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !ptr) {
        ptr = wl_seat_get_pointer(s);
        wl_pointer_add_listener(ptr, &ptr_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && ptr) {
        wl_pointer_destroy(ptr);
        ptr = NULL;
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d; (void)s; (void)n; }
static const struct wl_seat_listener seat_listener = {seat_caps, seat_name};

/* ---- registry ---------------------------------------------------------- */

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
        seat = wl_registry_bind(r, name, &wl_seat_interface, ver < 5 ? ver : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(iface, wl_output_interface.name) && n_out < MAX_OUT) {
        Out *o = &out[n_out++];
        memset(o, 0, sizeof *o);
        o->name = name;
        o->scale = 1;
        o->output = wl_registry_bind(r, name, &wl_output_interface, ver < 4 ? ver : 4);
        wl_output_add_listener(o->output, &out_listener, o);
        make_surface(o); /* a monitor plugged in while running */
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
    (void)d; (void)r;
    for (int i = 0; i < n_out; i++)
        if (out[i].name == name) {
            drop_surface(&out[i]);
            out[i] = out[--n_out];
            memset(&out[n_out], 0, sizeof out[n_out]);
            ptr_out = click_out = -1;
            return;
        }
}

static const struct wl_registry_listener reg_listener = {reg_global, reg_remove};

/* ---- backend ----------------------------------------------------------- */

static bool w_init(void)
{
    dpy = wl_display_connect(NULL);
    if (!dpy) return false;
    reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dpy);
    if (!comp || !shell) {
        lw_log("this Wayland desktop doesn't support the layer-shell protocol (GNOME doesn't), so livewall can't draw behind the windows");
        wl_display_disconnect(dpy);
        dpy = NULL;
        return false;
    }
    if (!egl_setup(EGL_PLATFORM_WAYLAND_KHR, dpy)) return false;
    for (int i = 0; i < n_out; i++) make_surface(&out[i]);
    wl_display_roundtrip(dpy); /* output scales */
    wl_display_roundtrip(dpy); /* first configure */
    lw_log("Wayland: %d monitor(s)", n_out);
    return true;
}

static int w_fd(void) { return wl_display_get_fd(dpy); }

static void w_dispatch(bool readable)
{
    if (readable) wl_display_dispatch(dpy);
    else wl_display_dispatch_pending(dpy);
    wl_display_flush(dpy);
}

static int w_outputs(void) { return n_out; }

static void frame_done(void *d, struct wl_callback *cb, uint32_t t)
{
    Out *o = d;
    (void)t;
    wl_callback_destroy(cb);
    o->frame_cb = NULL;
}
static const struct wl_callback_listener frame_listener = {frame_done};

static bool w_begin(int i, int *w, int *h)
{
    Out *o = &out[i];
    if (!o->configured || !o->egl_surf || o->frame_cb) return false;
    if (!eglMakeCurrent(g_egl_dpy, o->egl_surf, o->egl_surf, g_egl_ctx)) return false;
    *w = o->w * o->scale;
    *h = o->h * o->scale;
    return true;
}

static void w_end(int i)
{
    Out *o = &out[i];
    o->frame_cb = wl_surface_frame(o->surface);
    wl_callback_add_listener(o->frame_cb, &frame_listener, o);
    eglSwapInterval(g_egl_dpy, 0);
    eglSwapBuffers(g_egl_dpy, o->egl_surf); /* commits the surface */
}

static bool w_covered(void) { return false; } /* handled by frame callbacks */

static bool w_waiting(void)
{
    for (int i = 0; i < n_out; i++)
        if (out[i].configured && !out[i].frame_cb) return false;
    return true;
}

static void w_pointer(int i, Frame *f, double now)
{
    Out *o = &out[i];
    now_s = now;
    float s = (float)o->scale;
    f->mouse_on = ptr_out == i ? 1.0f : 0.0f;
    f->mouse[0] = ptr_x * s;
    f->mouse[1] = ptr_y * s;
    if (click_out == i) {
        f->click[0] = click_x * s;
        f->click[1] = click_y * s;
        f->click_age = (float)(now - click_at);
    } else {
        f->click_age = 1e6f;
    }
}

static void w_shutdown(void)
{
    if (!dpy) return;
    for (int i = 0; i < n_out; i++) drop_surface(&out[i]);
    n_out = 0;
    if (g_egl_dpy) { eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT); eglTerminate(g_egl_dpy); }
    wl_display_disconnect(dpy);
    dpy = NULL;
}

const Backend wayland_backend = {
    w_init, w_fd, w_dispatch, w_outputs, w_begin, w_end, w_covered, w_waiting, w_pointer, w_shutdown,
};
