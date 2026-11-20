/* config.c - ~/.config/livewall/config.ini. Same sections and keys as the
 * Windows version, so the format is documented in one place. */
#include "lw.h"
#include "builtin_shaders.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <time.h>
#include <sys/stat.h>
#include <math.h>

const Builtin g_builtins[] = {
    {"builtin:aurora",   "Aurora",    SRC_AURORA,   false},
    {"builtin:dunes",    "Dunes",     SRC_DUNES,    false},
    {"builtin:nightsky", "Night Sky", SRC_NIGHTSKY, false},
    {"builtin:ripples",  "Ripples",   SRC_RIPPLES,  true},
    {"builtin:glowgrid", "Glow Grid", SRC_GLOWGRID, true},
};
const int g_builtin_count = sizeof g_builtins / sizeof g_builtins[0];

float phase_at(double t)
{
    double cyc = t / PERIOD_S;
    cyc -= floor(cyc);
    return (float)(cyc * 6.283185307179586);
}

const char *config_dir(void)
{
    static char dir[PATH_LEN];
    if (!dir[0]) {
        const char *x = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
        if (x && *x) snprintf(dir, sizeof dir, "%s/livewall", x);
        else snprintf(dir, sizeof dir, "%s/.config/livewall", h ? h : ".");
        char parent[PATH_LEN];
        snprintf(parent, sizeof parent, "%s", dir);
        char *slash = strrchr(parent, '/');
        if (slash) { *slash = 0; mkdir(parent, 0755); }
        mkdir(dir, 0755);
    }
    return dir;
}

void lw_log(const char *fmt, ...)
{
    char path[PATH_LEN + 16];
    snprintf(path, sizeof path, "%s/livewall.log", config_dir());
    FILE *f = fopen(path, "a");
    va_list ap;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    if (f) {
        fprintf(f, "%s ", ts);
        va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f);
        fclose(f);
    }
    if (getenv("LIVEWALL_DEBUG")) {
        va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
        fputc('\n', stderr);
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->current, sizeof c->current, "%s", g_builtins[0].id);
    c->fps = 30;
    c->resolution = 0;
    c->speed = 100;
    c->brightness = 100;
    c->fit = FIT_COVER;
    c->pause_covered = c->pause_battery = c->autostart = true;
}

bool config_load(Config *c)
{
    char path[PATH_LEN + 16], line[PATH_LEN + 64], section[32] = "";
    snprintf(path, sizeof path, "%s/config.ini", config_dir());
    defaults(c);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    while (fgets(line, sizeof line, f)) {
        char *s = line, *e;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "\xEF\xBB\xBF", 3)) s += 3;
        for (e = s + strlen(s); e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        if (*s == ';' || *s == '#' || !*s) continue;
        if (*s == '[') {
            snprintf(section, sizeof section, "%.*s", (int)strcspn(s + 1, "]"), s + 1);
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = s, *v = eq + 1;
        int n = atoi(v);
        if (!strcmp(section, "wallpaper")) {
            if (!strcmp(k, "current")) snprintf(c->current, sizeof c->current, "%s", v);
            else if (!strcmp(k, "fit")) c->fit = (Fit)clampi(n, 0, 2);
            else if (!strcmp(k, "speed")) c->speed = clampi(n, 10, 400);
            else if (!strcmp(k, "brightness")) c->brightness = clampi(n, 10, 100);
        } else if (!strcmp(section, "engine")) {
            if (!strcmp(k, "fps")) c->fps = clampi(n, 1, 240);
            else if (!strcmp(k, "resolution")) c->resolution = clampi(n, 0, 4);
            else if (!strcmp(k, "pause_when_covered")) c->pause_covered = n != 0;
            else if (!strcmp(k, "pause_on_battery")) c->pause_battery = n != 0;
            else if (!strcmp(k, "autostart")) c->autostart = n != 0;
        } else if (!strcmp(section, "library")) {
            if (!strncmp(k, "item", 4) && c->lib_count < LIB_MAX)
                snprintf(c->lib[c->lib_count++], PATH_LEN, "%s", v);
        }
    }
    fclose(f);
    return true;
}

void config_save(const Config *c)
{
    char path[PATH_LEN + 16], tmp[PATH_LEN + 32];
    snprintf(path, sizeof path, "%s/config.ini", config_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "[wallpaper]\ncurrent=%s\nfit=%d\nspeed=%d\nbrightness=%d\n\n",
            c->current, (int)c->fit, c->speed, c->brightness);
    fprintf(f, "[engine]\nfps=%d\nresolution=%d\npause_when_covered=%d\npause_on_battery=%d\nautostart=%d\n\n",
            c->fps, c->resolution, c->pause_covered, c->pause_battery, c->autostart);
    fprintf(f, "[library]\ncount=%d\n", c->lib_count);
    for (int i = 0; i < c->lib_count; i++) fprintf(f, "item%d=%s\n", i, c->lib[i]);
    fclose(f);
    rename(tmp, path); /* atomic: the engine never reads half a file */
}

static bool ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !strcasecmp(s + a - b, ext);
}

Kind kind_of(const char *id)
{
    if (!strncmp(id, "builtin:", 8)) return builtin_find(id) ? KIND_SHADER : KIND_UNKNOWN;
    if (ends_with(id, ".glsl") || ends_with(id, ".frag")) return KIND_CUSTOM_SHADER;
    static const char *media[] = {".mp4", ".mkv", ".webm", ".mov", ".avi", ".wmv", ".m4v",
                                  ".gif", ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".avif", ".jxl"};
    for (size_t i = 0; i < sizeof media / sizeof *media; i++)
        if (ends_with(id, media[i])) return KIND_MEDIA;
    return KIND_UNKNOWN;
}

const Builtin *builtin_find(const char *id)
{
    for (int i = 0; i < g_builtin_count; i++)
        if (!strcmp(g_builtins[i].id, id)) return &g_builtins[i];
    return NULL;
}

bool lib_add(Config *c, const char *path)
{
    for (int i = 0; i < c->lib_count; i++)
        if (!strcmp(c->lib[i], path)) return true;
    if (c->lib_count >= LIB_MAX) return false;
    snprintf(c->lib[c->lib_count++], PATH_LEN, "%s", path);
    return true;
}
