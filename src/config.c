/* config.c - config.ini read/write via the Win32 profile API (no parser). */
#include "common.h"
#include "config.h"

const Builtin g_builtins[] = {
    {L"builtin:aurora", L"Aurora", FALSE},
    {L"builtin:dunes", L"Dunes", FALSE},
    {L"builtin:nightsky", L"Night Sky", FALSE},
    {L"builtin:ripples", L"Ripples", TRUE},
    {L"builtin:glowgrid", L"Glow Grid", TRUE},
};
const UINT g_builtin_count = sizeof g_builtins / sizeof g_builtins[0];

static void ini_path(WCHAR *p)
{
    app_dir(p);
    str_cat(p, L"\\config.ini", MAX_PATH);
}

static UINT get_uint(const WCHAR *path, const WCHAR *sec, const WCHAR *key, UINT def, UINT lo, UINT hi)
{
    UINT v = GetPrivateProfileIntW(sec, key, def, path);
    return v < lo || v > hi ? def : v;
}

static void put_uint(const WCHAR *path, const WCHAR *sec, const WCHAR *key, UINT v)
{
    WCHAR b[16];
    wsprintfW(b, L"%u", v);
    WritePrivateProfileStringW(sec, key, b, path);
}

BOOL config_load(Config *c)
{
    WCHAR path[MAX_PATH], key[16];
    UINT i, n;
    BOOL exists;

    ini_path(path);
    exists = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;

    GetPrivateProfileStringW(L"wallpaper", L"current", g_builtins[0].id, c->current, MAX_PATH, path);
    c->fit = (Fit)get_uint(path, L"wallpaper", L"fit", FIT_COVER, 0, 2);
    c->speed = get_uint(path, L"wallpaper", L"speed", 100, 10, 400);
    c->brightness = get_uint(path, L"wallpaper", L"brightness", 100, 10, 100);
    c->fps = get_uint(path, L"engine", L"fps", 30, 1, 240);
    /* "resolution" replaced the old "scale" key (default half) in 0.1.1, so
       existing installs move to the new default, Auto. */
    c->scale = get_uint(path, L"engine", L"resolution", 0, 0, 4);
    c->pause_covered = get_uint(path, L"engine", L"pause_when_covered", 1, 0, 1);
    c->pause_battery = get_uint(path, L"engine", L"pause_on_battery", 1, 0, 1);
    c->low_power = get_uint(path, L"engine", L"integrated_gpu", 0, 0, 1);
    c->autostart = get_uint(path, L"engine", L"start_with_windows", 1, 0, 1);
    GetPrivateProfileStringW(L"engine", L"autostart_exe", L"", c->autostart_exe, MAX_PATH + 16, path);
    GetPrivateProfileStringW(L"lockscreen", L"wallpaper", L"", c->lock, MAX_PATH, path);
    c->lock_picture = get_uint(path, L"lockscreen", L"match_picture", 0, 0, 1);

    n = get_uint(path, L"library", L"count", 0, 0, LIB_MAX);
    c->lib_count = 0;
    for (i = 0; i < n; i++) {
        wsprintfW(key, L"%u", i);
        GetPrivateProfileStringW(L"library", key, L"", c->lib[c->lib_count], MAX_PATH, path);
        if (c->lib[c->lib_count][0])
            c->lib_count++;
    }
    return exists;
}

void config_save(const Config *c)
{
    WCHAR path[MAX_PATH], key[16];
    UINT i;

    ini_path(path);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        /* A UTF-16 BOM makes the profile API store Unicode, so non-ASCII
           paths survive. */
        HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            static const BYTE bom[2] = {0xFF, 0xFE};
            DWORD w;
            WriteFile(f, bom, 2, &w, NULL);
            CloseHandle(f);
        }
    }

    WritePrivateProfileStringW(L"wallpaper", L"current", c->current, path);
    put_uint(path, L"wallpaper", L"fit", c->fit);
    put_uint(path, L"wallpaper", L"speed", c->speed);
    put_uint(path, L"wallpaper", L"brightness", c->brightness);
    put_uint(path, L"engine", L"fps", c->fps);
    put_uint(path, L"engine", L"resolution", c->scale);
    WritePrivateProfileStringW(L"engine", L"scale", NULL, path); /* pre-0.1.1 key */
    put_uint(path, L"engine", L"pause_when_covered", c->pause_covered ? 1 : 0);
    put_uint(path, L"engine", L"pause_on_battery", c->pause_battery ? 1 : 0);
    put_uint(path, L"engine", L"integrated_gpu", c->low_power ? 1 : 0);
    put_uint(path, L"engine", L"start_with_windows", c->autostart ? 1 : 0);
    WritePrivateProfileStringW(L"engine", L"autostart_exe", c->autostart_exe, path);
    WritePrivateProfileStringW(L"lockscreen", L"wallpaper", c->lock, path);
    put_uint(path, L"lockscreen", L"match_picture", c->lock_picture ? 1 : 0);

    WritePrivateProfileStringW(L"library", NULL, NULL, path); /* rewrite the whole section */
    put_uint(path, L"library", L"count", c->lib_count);
    for (i = 0; i < c->lib_count; i++) {
        wsprintfW(key, L"%u", i);
        WritePrivateProfileStringW(L"library", key, c->lib[i], path);
    }
}

Kind kind_of(const WCHAR *id)
{
    static const WCHAR *video[] = {L".mp4", L".m4v", L".mov", L".mkv", L".webm", L".wmv", L".avi"};
    static const WCHAR *image[] = {L".png", L".jpg", L".jpeg", L".bmp", L".webp", L".tif", L".tiff", L".jxr"};
    const WCHAR *e;
    UINT i;

    if (str_starts(id, L"builtin:"))
        return builtin_find(id) ? KIND_SHADER : KIND_UNKNOWN;
    e = path_ext(id);
    if (str_ieq(e, L".hlsl") || str_ieq(e, L".fx"))
        return KIND_CUSTOM_SHADER;
    if (str_ieq(e, L".gif"))
        return KIND_GIF;
    for (i = 0; i < sizeof video / sizeof video[0]; i++)
        if (str_ieq(e, video[i]))
            return KIND_VIDEO;
    for (i = 0; i < sizeof image / sizeof image[0]; i++)
        if (str_ieq(e, image[i]))
            return KIND_IMAGE;
    return KIND_UNKNOWN;
}

const WCHAR *kind_label(Kind k)
{
    switch (k) {
    case KIND_SHADER: return L"Shader";
    case KIND_CUSTOM_SHADER: return L"Shader";
    case KIND_VIDEO: return L"Video";
    case KIND_GIF: return L"GIF";
    case KIND_IMAGE: return L"Image";
    default: return L"File";
    }
}

const Builtin *builtin_find(const WCHAR *id)
{
    UINT i;
    for (i = 0; i < g_builtin_count; i++)
        if (str_ieq(id, g_builtins[i].id))
            return &g_builtins[i];
    return NULL;
}

const WCHAR *display_name(const WCHAR *id)
{
    const Builtin *b = builtin_find(id);
    return b ? b->name : path_file(id);
}

BOOL lib_add(Config *c, const WCHAR *path)
{
    UINT i;
    if (kind_of(path) == KIND_UNKNOWN || kind_of(path) == KIND_SHADER)
        return FALSE;
    for (i = 0; i < c->lib_count; i++)
        if (str_ieq(c->lib[i], path))
            return TRUE; /* already there */
    if (c->lib_count >= LIB_MAX)
        return FALSE;
    str_copy(c->lib[c->lib_count++], path, MAX_PATH);
    return TRUE;
}

void lib_remove(Config *c, UINT index)
{
    UINT i;
    if (index >= c->lib_count)
        return;
    for (i = index; i + 1 < c->lib_count; i++)
        str_copy(c->lib[i], c->lib[i + 1], MAX_PATH);
    c->lib_count--;
    c->lib[c->lib_count][0] = 0;
}
