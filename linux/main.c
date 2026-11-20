/* main.c - livewall for Linux: commands, the engine loop, -snapshot.
 *
 *   livewall                     start the wallpaper (if it isn't running)
 *   livewall -startup            same, used by the autostart entry
 *   livewall -set <id|file>      switch wallpaper (builtin:aurora, a video, a .glsl…)
 *   livewall -list               show the built-in wallpapers and your library
 *   livewall -pause / -resume    pause or resume drawing
 *   livewall -autostart on|off   start with the desktop or not
 *   livewall -quit               stop the wallpaper
 *   livewall -snapshot <id|file> <out.png> [seconds] [WxH]   render one frame offscreen
 *
 * The running engine re-reads config.ini on SIGHUP; the commands above write
 * the file and then send it. */
#include "lw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/file.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <math.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

EGLDisplay g_egl_dpy;
EGLContext g_egl_ctx;
EGLConfig g_egl_cfg;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* ---- EGL ---------------------------------------------------------------- */

bool egl_setup(EGLenum platform, void *native)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    g_egl_dpy = get_dpy && platform ? get_dpy(platform, native, NULL) : eglGetDisplay((EGLNativeDisplayType)native);
    if (g_egl_dpy == EGL_NO_DISPLAY || !eglInitialize(g_egl_dpy, NULL, NULL)) {
        lw_log("EGL: no display");
        return false;
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint want[] = {
        EGL_SURFACE_TYPE, platform == EGL_PLATFORM_SURFACELESS_MESA ? EGL_PBUFFER_BIT : EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g_egl_dpy, want, &g_egl_cfg, 1, &n) || n < 1) {
        want[1] = 0; /* surfaceless drivers may list no pbuffer configs */
        if (!eglChooseConfig(g_egl_dpy, want, &g_egl_cfg, 1, &n) || n < 1) {
            lw_log("EGL: no suitable config");
            return false;
        }
    }
    EGLint es3[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE}, es2[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    g_egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, es3);
    if (g_egl_ctx == EGL_NO_CONTEXT) g_egl_ctx = eglCreateContext(g_egl_dpy, g_egl_cfg, EGL_NO_CONTEXT, es2);
    if (g_egl_ctx == EGL_NO_CONTEXT) { lw_log("EGL: can't create an OpenGL ES context"); return false; }
    eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl_ctx);
    lw_log("GPU: %s (%s)", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
    return true;
}

/* ---- single instance ---------------------------------------------------- */

static void pid_path(char *out, size_t n)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (rt && *rt) snprintf(out, n, "%s/livewall.pid", rt);
    else snprintf(out, n, "%s/livewall.pid", config_dir());
}

static pid_t running_pid(void)
{
    char path[PATH_LEN];
    pid_path(path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    pid_t pid = 0;
    if (flock(fd, LOCK_EX | LOCK_NB) == 0) { flock(fd, LOCK_UN); close(fd); return 0; } /* stale */
    char buf[32] = {0};
    if (read(fd, buf, sizeof buf - 1) > 0) pid = (pid_t)atoi(buf);
    close(fd);
    return pid;
}

static int lock_fd = -1;

static bool take_lock(void)
{
    char path[PATH_LEN];
    pid_path(path, sizeof path);
    lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) return false;
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%d\n", (int)getpid());
    if (ftruncate(lock_fd, 0) == 0 && write(lock_fd, buf, (size_t)n) != n) return false;
    return true;
}

static void signal_engine(int sig)
{
    pid_t pid = running_pid();
    if (pid > 0) kill(pid, sig);
}

/* ---- autostart (XDG: works on KDE, GNOME on X11, XFCE, Cinnamon, MATE…) ---- */

static void set_autostart(bool on)
{
    char dir[PATH_LEN], path[PATH_LEN + 32], exe[PATH_MAX] = {0};
    const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    if (x && *x) snprintf(dir, sizeof dir, "%s/autostart", x);
    else snprintf(dir, sizeof dir, "%s/.config/autostart", h ? h : ".");
    snprintf(path, sizeof path, "%s/livewall.desktop", dir);
    if (!on) { unlink(path); return; }
    mkdir(dir, 0755);
    if (readlink("/proc/self/exe", exe, sizeof exe - 1) <= 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "[Desktop Entry]\nType=Application\nName=livewall\nComment=Live wallpaper\n"
               "Exec=\"%s\" -startup\nTerminal=false\nNoDisplay=true\nX-GNOME-Autostart-enabled=true\n", exe);
    fclose(f);
}

/* ---- battery ------------------------------------------------------------ */

static bool on_battery(void)
{
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return false;
    struct dirent *e;
    bool battery = false, mains_online = false;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[512], buf[32] = {0};
        snprintf(p, sizeof p, "/sys/class/power_supply/%s/type", e->d_name);
        FILE *f = fopen(p, "r");
        if (!f) continue;
        if (!fgets(buf, sizeof buf, f)) buf[0] = 0;
        fclose(f);
        if (!strncmp(buf, "Battery", 7)) {
            battery = true;
        } else if (!strncmp(buf, "Mains", 5) || !strncmp(buf, "USB", 3)) {
            snprintf(p, sizeof p, "/sys/class/power_supply/%s/online", e->d_name);
            f = fopen(p, "r");
            if (f) { if (fgets(buf, sizeof buf, f) && buf[0] == '1') mains_online = true; fclose(f); }
        }
    }
    closedir(d);
    return battery && !mains_online;
}

/* ---- the engine --------------------------------------------------------- */

static int pick_scale(int w, int h, int res)
{
    if (res == 1) return 1;
    if (res >= 2) return res;
    long px = (long)w * h, budget = 2560L * 1600L; /* Auto: full resolution up to 2560x1600 */
    int s = 1;
    while (px / ((long)s * s) > budget) s++;
    return s;
}

typedef struct {
    Config cfg;
    Kind kind;
    Shader *shader;
    bool user_paused, paused;
    double anim, last;
} Engine;

static Engine E;
static const Backend *B;
static int wake_fd = -1;

static void open_wallpaper(void)
{
    eglMakeCurrent(g_egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, g_egl_ctx); /* GL objects are shared by every surface */
    shader_free(E.shader);
    E.shader = NULL;
    media_close();
    E.kind = kind_of(E.cfg.current);
    if (E.kind == KIND_SHADER || E.kind == KIND_CUSTOM_SHADER) {
        E.shader = shader_load(E.cfg.current);
        if (!E.shader) lw_log("could not show %s", E.cfg.current);
    } else if (E.kind == KIND_MEDIA) {
        if (!media_open(E.cfg.current, E.cfg.fit, wake_fd)) lw_log("could not show %s", E.cfg.current);
    } else {
        lw_log("unknown wallpaper %s", E.cfg.current);
    }
    lw_log("wallpaper: %s", E.cfg.current);
}

static void draw_all(double now)
{
    for (int i = 0; i < B->outputs(); i++) {
        int w, h;
        if (!B->begin(i, &w, &h)) continue;
        Frame f = {0};
        f.time = E.anim;
        f.phase = phase_at(E.anim);
        B->pointer(i, &f, now);
        if (E.kind == KIND_MEDIA) media_draw(0, w, h, true);
        else shader_draw(E.shader, w, h, &f, pick_scale(w, h, E.cfg.resolution));
        glViewport(0, 0, w, h);
        dim_draw(E.cfg.brightness / 100.0f);
        B->end(i);
    }
}

static void arm(int tfd, double interval)
{
    struct itimerspec it = {0};
    if (interval > 0) {
        long ns = (long)(interval * 1e9);
        it.it_interval.tv_sec = ns / 1000000000L;
        it.it_interval.tv_nsec = ns % 1000000000L;
        it.it_value = it.it_interval;
    }
    timerfd_settime(tfd, 0, &it, NULL);
}

static int engine_run(bool quiet)
{
    (void)quiet;
    if (running_pid() > 0) return 0; /* already running: nothing to do */
    if (!take_lock()) return 0;
    bool first = !config_load(&E.cfg);
    if (first) config_save(&E.cfg);
    if (first && E.cfg.autostart) set_autostart(true);

    const char *wl = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    B = NULL;
    if (wl && *wl && wayland_backend.init()) B = &wayland_backend;
    else if (x && *x && x11_backend.init()) B = &x11_backend;
    if (!B) {
        fprintf(stderr, "livewall: can't draw behind the desktop here (see ~/.config/livewall/livewall.log)\n");
        lw_log("no usable desktop (needs Wayland with layer-shell, or X11)");
        return 1;
    }

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP); sigaddset(&set, SIGTERM); sigaddset(&set, SIGINT);
    sigaddset(&set, SIGUSR1); sigaddset(&set, SIGUSR2);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, SFD_CLOEXEC);
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);

    open_wallpaper();
    E.last = now_s();
    double next_poll = 0;
    bool timer_on = false, quit = false;

    while (!quit) {
        double now = now_s();
        if (now >= next_poll) {
            bool covered = E.cfg.pause_covered && B->covered();
            bool battery = E.cfg.pause_battery && on_battery();
            bool p = E.user_paused || covered || battery;
            if (p != E.paused) {
                E.paused = p;
                if (E.kind == KIND_MEDIA) media_pause(p);
                lw_log(p ? "paused%s%s%s" : "running", E.user_paused ? " (by you)" : "",
                       covered ? " (desktop covered)" : "", battery ? " (on battery)" : "");
            }
            next_poll = now + 0.5;
        }
        /* Shaders tick on a timer; media draws when mpv has a new frame.
           The timer is off while paused or while the compositor holds us. */
        bool want_timer = !E.paused && E.kind != KIND_MEDIA && E.shader && !B->waiting();
        if (want_timer != timer_on) { arm(tfd, want_timer ? 1.0 / E.cfg.fps : 0); timer_on = want_timer; E.last = now; }

        B->dispatch(false);
        struct pollfd pf[4] = {{B->fd(), POLLIN, 0}, {sfd, POLLIN, 0}, {tfd, POLLIN, 0}, {wake_fd, POLLIN, 0}};
        int timeout = (int)((next_poll - now) * 1000) + 1;
        if (poll(pf, 4, timeout < 1 ? 1 : timeout) < 0 && errno != EINTR) break;
        now = now_s();

        if (pf[0].revents & POLLIN) B->dispatch(true);
        if (pf[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sfd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGHUP) {
                    char old[PATH_LEN];
                    Fit old_fit = E.cfg.fit;
                    snprintf(old, sizeof old, "%s", E.cfg.current);
                    config_load(&E.cfg);
                    if (strcmp(old, E.cfg.current) || (E.kind == KIND_MEDIA && old_fit != E.cfg.fit)) open_wallpaper();
                    if (E.kind == KIND_MEDIA) media_pause(E.paused);
                    timer_on = false; /* re-arm with the new fps */
                    next_poll = 0;
                    if (E.kind != KIND_MEDIA) draw_all(now);
                } else if (si.ssi_signo == SIGUSR1) { E.user_paused = true; next_poll = 0; }
                else if (si.ssi_signo == SIGUSR2) { E.user_paused = false; next_poll = 0; }
                else quit = true;
            }
        }
        if (pf[2].revents & POLLIN) {
            uint64_t ticks;
            if (read(tfd, &ticks, sizeof ticks) > 0 && !E.paused) {
                E.anim += (now - E.last) * E.cfg.speed / 100.0;
                if (E.anim > 86400.0) E.anim -= 86400.0;
                E.last = now;
                draw_all(now);
            }
        }
        if (pf[3].revents & POLLIN) {
            uint64_t v;
            while (read(wake_fd, &v, sizeof v) > 0) {}
            media_service();
            if (media_has_frame() && !E.paused) draw_all(now);
        }
    }

    lw_log("exiting");
    media_close();
    B->shutdown();
    return 0;
}

/* ---- -snapshot ---------------------------------------------------------- */

/* A pretend cursor, the same as the Windows version's preview.c: it drifts
   around the middle and clicks every 3 s, so frame series show the
   interactive wallpapers reacting. */
static void fake_mouse(Frame *f, int w, int h, double at)
{
    double t = at - 8.4, age = fmod(fmod(t, 3.0) + 3.0, 3.0), c = t - age;
    f->mouse[0] = w * 0.5f + w * 0.25f * (float)sin(2 * M_PI * t / 6.0);
    f->mouse[1] = h * 0.5f + h * 0.15f * (float)sin(2 * M_PI * t / 3.0);
    f->click[0] = w * 0.5f + w * 0.25f * (float)sin(2 * M_PI * c / 6.0);
    f->click[1] = h * 0.5f + h * 0.15f * (float)sin(2 * M_PI * c / 3.0);
    f->click_age = (float)age;
    f->mouse_on = 1.0f;
}

static int snapshot(const char *id, const char *out, double at, int w, int h)
{
    Kind k = kind_of(id);
    if (k == KIND_UNKNOWN) { fprintf(stderr, "livewall: unknown wallpaper %s\n", id); return 1; }
    if (!egl_setup(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY) &&
        !egl_setup(0, EGL_DEFAULT_DISPLAY)) {
        fprintf(stderr, "livewall: no OpenGL ES available\n");
        return 1;
    }
    GLuint fbo, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "livewall: no framebuffer\n"); return 1; }

    bool ok = false;
    if (k == KIND_MEDIA) {
        int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (media_open(id, FIT_COVER, fd)) {
            bool seeked = at <= 0.0;
            double until = now_s() + 10.0;
            while (now_s() < until && !ok) {
                struct pollfd pf = {fd, POLLIN, 0};
                poll(&pf, 1, 100);
                uint64_t v;
                while (read(fd, &v, sizeof v) > 0) {}
                media_service();
                if (media_has_frame()) {
                    if (!seeked) { media_seek(at); seeked = true; continue; }
                    media_draw((int)fbo, w, h, false);
                    ok = true;
                }
            }
            if (!ok) fprintf(stderr, "livewall: no frame from libmpv\n");
        } else {
            fprintf(stderr, "livewall: can't open %s (is libmpv installed?)\n", id);
        }
    } else {
        Shader *s = shader_load(id);
        if (s) {
            Frame f = {0};
            f.time = at;
            f.phase = phase_at(at);
            fake_mouse(&f, w, h, at);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            shader_draw(s, w, h, &f, 1);
            ok = true;
        } else {
            fprintf(stderr, "livewall: shader error (see ~/.config/livewall/livewall.log)\n");
        }
    }
    if (!ok) return 1;

    uint8_t *px = malloc((size_t)w * h * 4), *flip = malloc((size_t)w * h * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) {
        memcpy(flip + (size_t)y * w * 4, px + (size_t)(h - 1 - y) * w * 4, (size_t)w * 4);
        for (int x = 0; x < w; x++) flip[((size_t)y * w + x) * 4 + 3] = 255;
    }
    ok = png_write(out, flip, w, h);
    free(px);
    free(flip);
    media_close();
    return ok ? 0 : 1;
}

/* ---- commands ----------------------------------------------------------- */

static void usage(void)
{
    puts("livewall " LW_VERSION " - live wallpaper\n\n"
         "  livewall                     start the wallpaper\n"
         "  livewall -set <id|file>      switch wallpaper (see -list), or a video, GIF, picture or .glsl file\n"
         "  livewall -list               show the built-in wallpapers and your library\n"
         "  livewall -speed <10-400>     animation speed in percent\n"
         "  livewall -brightness <10-100>\n"
         "  livewall -fps <1-240>        frame rate cap\n"
         "  livewall -fit cover|contain|stretch   for videos and pictures\n"
         "  livewall -pause / -resume\n"
         "  livewall -autostart on|off   start with the desktop\n"
         "  livewall -quit               stop the wallpaper\n"
         "  livewall -snapshot <id|file> <out.png> [seconds] [WxH]");
}

static int set_and_reload(Config *c)
{
    config_save(c);
    signal_engine(SIGHUP);
    return 0;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "";
    while (*cmd == '-') cmd++;
    Config c;

    if (!*cmd || !strcmp(cmd, "startup")) return engine_run(!strcmp(cmd, "startup"));
    if (!strcmp(cmd, "help") || !strcmp(cmd, "h") || !strcmp(cmd, "version")) { usage(); return 0; }
    if (!strcmp(cmd, "quit")) {
        pid_t pid = running_pid();
        if (pid <= 0) return 0;
        kill(pid, SIGTERM);
        for (int i = 0; i < 50 && running_pid() > 0; i++) usleep(100000);
        return 0;
    }
    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume")) { signal_engine(!strcmp(cmd, "pause") ? SIGUSR1 : SIGUSR2); return 0; }
    if (!strcmp(cmd, "list")) {
        config_load(&c);
        for (int i = 0; i < g_builtin_count; i++)
            printf("%s %-18s %s%s\n", strcmp(c.current, g_builtins[i].id) ? " " : "*", g_builtins[i].id,
                   g_builtins[i].name, g_builtins[i].interactive ? " (follows the mouse)" : "");
        for (int i = 0; i < c.lib_count; i++) printf("%s %s\n", strcmp(c.current, c.lib[i]) ? " " : "*", c.lib[i]);
        return 0;
    }
    if (!strcmp(cmd, "set") && argc > 2) {
        char id[PATH_LEN];
        if (!strchr(argv[2], '/') && !strchr(argv[2], '.') && strncmp(argv[2], "builtin:", 8))
            snprintf(id, sizeof id, "builtin:%s", argv[2]); /* "livewall -set dunes" */
        else if (argv[2][0] != '/' && strncmp(argv[2], "builtin:", 8)) {
            char *abs = realpath(argv[2], NULL);
            snprintf(id, sizeof id, "%s", abs ? abs : argv[2]);
            free(abs);
        } else snprintf(id, sizeof id, "%s", argv[2]);
        if (kind_of(id) == KIND_UNKNOWN) { fprintf(stderr, "livewall: don't know how to show %s\n", argv[2]); return 1; }
        config_load(&c);
        snprintf(c.current, sizeof c.current, "%s", id);
        if (strncmp(id, "builtin:", 8)) lib_add(&c, id);
        return set_and_reload(&c);
    }
    if ((!strcmp(cmd, "speed") || !strcmp(cmd, "brightness") || !strcmp(cmd, "fps") || !strcmp(cmd, "resolution")) && argc > 2) {
        config_load(&c);
        int v = atoi(argv[2]);
        if (!strcmp(cmd, "speed")) c.speed = v < 10 ? 10 : v > 400 ? 400 : v;
        else if (!strcmp(cmd, "brightness")) c.brightness = v < 10 ? 10 : v > 100 ? 100 : v;
        else if (!strcmp(cmd, "fps")) c.fps = v < 1 ? 1 : v > 240 ? 240 : v;
        else c.resolution = v < 0 ? 0 : v > 4 ? 4 : v;
        return set_and_reload(&c);
    }
    if (!strcmp(cmd, "fit") && argc > 2) {
        config_load(&c);
        c.fit = !strcmp(argv[2], "contain") ? FIT_CONTAIN : !strcmp(argv[2], "stretch") ? FIT_STRETCH : FIT_COVER;
        return set_and_reload(&c);
    }
    if (!strcmp(cmd, "autostart") && argc > 2) {
        config_load(&c);
        c.autostart = !strcmp(argv[2], "on");
        set_autostart(c.autostart);
        config_save(&c);
        return 0;
    }
    if (!strcmp(cmd, "snapshot") && argc > 3) {
        char id[PATH_LEN];
        if (!strchr(argv[2], '/') && !strchr(argv[2], '.') && strncmp(argv[2], "builtin:", 8))
            snprintf(id, sizeof id, "builtin:%s", argv[2]);
        else snprintf(id, sizeof id, "%s", argv[2]);
        int w = 960, h = 540;
        if (argc > 5) sscanf(argv[5], "%dx%d", &w, &h);
        return snapshot(id, argv[3], argc > 4 ? atof(argv[4]) : 9.0, w, h);
    }
    usage();
    return 1;
}
