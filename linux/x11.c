/* x11.c - a desktop-type window under everything else (X11 desktops).
 *
 * One window covers the whole screen (all monitors), like the Windows
 * version's single swap chain. It is marked _NET_WM_WINDOW_TYPE_DESKTOP and
 * "below", so window managers keep it at the bottom and out of the taskbar.
 *
 * "Covered" means the active window is fullscreen or maximized. */
#include "lw.h"
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *dpy;
static Window win, root;
static EGLSurface surf;
static int sw, sh;
static float click_x, click_y;
static double click_at = -1e9, now_s;
static Atom a_active, a_state, a_fs, a_max_v, a_max_h, a_hidden;

static Atom atom(const char *n) { return XInternAtom(dpy, n, False); }

static bool x_init(void)
{
    dpy = XOpenDisplay(NULL);
    if (!dpy) return false;
    int scr = DefaultScreen(dpy);
    root = RootWindow(dpy, scr);
    sw = DisplayWidth(dpy, scr);
    sh = DisplayHeight(dpy, scr);
    if (!egl_setup(EGL_PLATFORM_X11_KHR, dpy)) return false;

    EGLint vid = 0;
    eglGetConfigAttrib(g_egl_dpy, g_egl_cfg, EGL_NATIVE_VISUAL_ID, &vid);
    XVisualInfo tmpl = {.visualid = (VisualID)vid}, *vi;
    int n = 0;
    vi = XGetVisualInfo(dpy, VisualIDMask, &tmpl, &n);
    XSetWindowAttributes wa = {0};
    wa.colormap = XCreateColormap(dpy, root, vi ? vi->visual : DefaultVisual(dpy, scr), AllocNone);
    wa.event_mask = ButtonPressMask | StructureNotifyMask | ExposureMask;
    wa.background_pixel = 0;
    win = XCreateWindow(dpy, root, 0, 0, (unsigned)sw, (unsigned)sh, 0,
                        vi ? vi->depth : DefaultDepth(dpy, scr), InputOutput,
                        vi ? vi->visual : DefaultVisual(dpy, scr),
                        CWColormap | CWEventMask | CWBackPixel, &wa);
    if (vi) XFree(vi);

    Atom type = atom("_NET_WM_WINDOW_TYPE_DESKTOP");
    XChangeProperty(dpy, win, atom("_NET_WM_WINDOW_TYPE"), XA_ATOM, 32, PropModeReplace, (unsigned char *)&type, 1);
    Atom st[] = {atom("_NET_WM_STATE_BELOW"), atom("_NET_WM_STATE_STICKY"),
                 atom("_NET_WM_STATE_SKIP_TASKBAR"), atom("_NET_WM_STATE_SKIP_PAGER")};
    XChangeProperty(dpy, win, atom("_NET_WM_STATE"), XA_ATOM, 32, PropModeReplace, (unsigned char *)st, 4);
    long all = 0xFFFFFFFF; /* every virtual desktop */
    XChangeProperty(dpy, win, atom("_NET_WM_DESKTOP"), XA_CARDINAL, 32, PropModeReplace, (unsigned char *)&all, 1);
    XStoreName(dpy, win, "livewall");
    XClassHint ch = {"livewall", "livewall"};
    XSetClassHint(dpy, win, &ch);
    XMapWindow(dpy, win);
    XLowerWindow(dpy, win);
    XFlush(dpy);

    surf = eglCreateWindowSurface(g_egl_dpy, g_egl_cfg, (EGLNativeWindowType)win, NULL);
    if (surf == EGL_NO_SURFACE) { lw_log("X11: can't create the drawing surface"); return false; }
    a_active = atom("_NET_ACTIVE_WINDOW");
    a_state = atom("_NET_WM_STATE");
    a_fs = atom("_NET_WM_STATE_FULLSCREEN");
    a_max_v = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    a_max_h = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    a_hidden = atom("_NET_WM_STATE_HIDDEN");
    lw_log("X11: %dx%d", sw, sh);
    return true;
}

static int x_fd(void) { return ConnectionNumber(dpy); }

static void x_dispatch(bool readable)
{
    (void)readable;
    while (XPending(dpy)) {
        XEvent e;
        XNextEvent(dpy, &e);
        if (e.type == ButtonPress) {
            click_x = (float)e.xbutton.x;
            click_y = (float)e.xbutton.y;
            click_at = now_s;
        } else if (e.type == ConfigureNotify && e.xconfigure.window == win) {
            sw = e.xconfigure.width;
            sh = e.xconfigure.height;
        }
    }
    XFlush(dpy);
}

static int x_outputs(void) { return 1; }

static bool x_begin(int i, int *w, int *h)
{
    (void)i;
    if (!eglMakeCurrent(g_egl_dpy, surf, surf, g_egl_ctx)) return false;
    *w = sw;
    *h = sh;
    return true;
}

static void x_end(int i)
{
    (void)i;
    eglSwapInterval(g_egl_dpy, 0);
    eglSwapBuffers(g_egl_dpy, surf);
}

static bool window_hides_desktop(Window w)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    bool fs = false, mv = false, mh = false, hidden = false;
    if (XGetWindowProperty(dpy, w, a_state, 0, 64, False, XA_ATOM, &type, &fmt, &n, &after, &data) == Success && data) {
        Atom *a = (Atom *)data;
        for (unsigned long i = 0; i < n; i++) {
            if (a[i] == a_fs) fs = true;
            else if (a[i] == a_max_v) mv = true;
            else if (a[i] == a_max_h) mh = true;
            else if (a[i] == a_hidden) hidden = true;
        }
        XFree(data);
    }
    return !hidden && (fs || (mv && mh));
}

static bool x_covered(void)
{
    Atom type;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = NULL;
    bool covered = false;
    if (XGetWindowProperty(dpy, root, a_active, 0, 1, False, XA_WINDOW, &type, &fmt, &n, &after, &data) == Success && data) {
        Window active = n ? *(Window *)data : None;
        XFree(data);
        if (active && active != win) covered = window_hides_desktop(active);
    }
    return covered;
}

static bool x_waiting(void) { return false; }

static void x_pointer(int i, Frame *f, double now)
{
    (void)i;
    Window r, child;
    int rx, ry, wx, wy;
    unsigned mask;
    now_s = now;
    if (XQueryPointer(dpy, root, &r, &child, &rx, &ry, &wx, &wy, &mask)) {
        f->mouse[0] = (float)rx;
        f->mouse[1] = (float)ry;
        f->mouse_on = (child == None || child == win) ? 1.0f : 0.0f;
    }
    f->click[0] = click_x;
    f->click[1] = click_y;
    f->click_age = (float)(now - click_at);
}

static void x_shutdown(void)
{
    if (!dpy) return;
    if (g_egl_dpy) {
        eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surf) eglDestroySurface(g_egl_dpy, surf);
        eglTerminate(g_egl_dpy);
    }
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);
    dpy = NULL;
}

const Backend x11_backend = {
    x_init, x_fd, x_dispatch, x_outputs, x_begin, x_end, x_covered, x_waiting, x_pointer, x_shutdown,
};
