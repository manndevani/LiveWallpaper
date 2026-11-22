/* util.c - the few C runtime pieces we need, plus small shared helpers.
 * The program links without the C runtime (/NODEFAULTLIB), so anything the
 * compiler emits calls to (memset, memcpy, memcmp, _fltused) lives here. */
#include "common.h"
#include <intrin.h>
#include <xmmintrin.h>
#include <stdarg.h>
#include <string.h>

int _fltused = 0;

/* The exe's load config table, which the C runtime would normally provide.
   DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32 makes Windows take the
   DLLs livewall links against from System32 only, never from the folder
   livewall.exe is in (see /DEPENDENTLOADFLAG in build.bat). */
const IMAGE_LOAD_CONFIG_DIRECTORY _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY),
    .DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32,
};

#pragma function(memset)
void *memset(void *d, int c, size_t n)
{
    __stosb((unsigned char *)d, (unsigned char)c, n);
    return d;
}

#pragma function(memcpy)
void *memcpy(void *d, const void *s, size_t n)
{
    __movsb((unsigned char *)d, (const unsigned char *)s, n);
    return d;
}

#pragma function(memcmp)
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x - *y;
    return 0;
}

void *mem_alloc(SIZE_T n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }

void mem_free(void *p)
{
    if (p)
        HeapFree(GetProcessHeap(), 0, p);
}

/* ---- strings and paths ------------------------------------------------- */

int str_len(const WCHAR *s)
{
    int n = 0;
    if (s)
        while (s[n])
            n++;
    return n;
}

void str_copy(WCHAR *d, const WCHAR *s, int cap)
{
    int i = 0;
    if (cap <= 0)
        return;
    for (; s && s[i] && i < cap - 1; i++)
        d[i] = s[i];
    d[i] = 0;
}

void str_cat(WCHAR *d, const WCHAR *s, int cap)
{
    int n = str_len(d);
    if (n < cap)
        str_copy(d + n, s, cap - n);
}

BOOL str_ieq(const WCHAR *a, const WCHAR *b) { return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL; }

BOOL str_starts(const WCHAR *s, const WCHAR *prefix)
{
    int n = str_len(prefix);
    return str_len(s) >= n && CompareStringOrdinal(s, n, prefix, n, TRUE) == CSTR_EQUAL;
}

const WCHAR *path_file(const WCHAR *p)
{
    const WCHAR *f = p;
    for (; *p; p++)
        if (*p == '\\' || *p == '/')
            f = p + 1;
    return f;
}

const WCHAR *path_ext(const WCHAR *p)
{
    const WCHAR *e = NULL;
    for (p = path_file(p); *p; p++)
        if (*p == '.')
            e = p;
    return e ? e : L"";
}

void app_dir(WCHAR *out)
{
    DWORD n = GetEnvironmentVariableW(L"APPDATA", out, MAX_PATH);
    if (!n || n >= MAX_PATH - 32) {
        n = GetTempPathW(MAX_PATH, out);
        if (n && out[n - 1] == '\\')
            out[n - 1] = 0;
    }
    str_cat(out, L"\\livewall", MAX_PATH);
    CreateDirectoryW(out, NULL);
}

/* ---- log file: %APPDATA%\livewall\livewall.log, capped at 512 KB -------- */

static void log_bytes(const char *s, int n)
{
    static WCHAR path[MAX_PATH];
    LARGE_INTEGER size;
    DWORD written;
    HANDLE f;

    app_dir(path);
    str_cat(path, L"\\livewall.log", MAX_PATH);
    f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 512 * 1024) {
        CloseHandle(f);
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE)
            return;
    }
    WriteFile(f, s, (DWORD)n, &written, NULL);
    CloseHandle(f);
}

void log_line(const WCHAR *fmt, ...)
{
    static WCHAR msg[1025], line[1100];
    static char utf8[3400];
    SYSTEMTIME t;
    va_list ap;
    int n;

    va_start(ap, fmt);
    wvsprintfW(msg, fmt, ap);
    va_end(ap);
    GetLocalTime(&t);
    wsprintfW(line, L"%04u-%02u-%02u %02u:%02u:%02u  [%u] ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
              t.wSecond, GetCurrentProcessId());
    str_cat(line, msg, 1096);
    str_cat(line, L"\r\n", 1100);
    OutputDebugStringW(line);
    n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof utf8, NULL, NULL);
    if (n > 1)
        log_bytes(utf8, n - 1);
}

void log_text(const char *text, int len)
{
    if (text && len > 0)
        log_bytes(text, len);
}

/* ---- app icon, drawn in code so there is no resource file -------------- */

static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }
static float absf(float x) { return x < 0 ? -x : x; }
static float sqrtf_(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }

/* Rounded square with a teal-to-violet gradient and a light aurora band. */
HICON make_app_icon(int n)
{
    BITMAPV5HEADER bi;
    void *bits = NULL;
    HBITMAP color, mask;
    ICONINFO ii;
    HICON icon;
    HDC dc;
    int x, y;

    memset(&bi, 0, sizeof bi);
    bi.bV5Size = sizeof bi;
    bi.bV5Width = n;
    bi.bV5Height = -n;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    dc = GetDC(NULL);
    color = CreateDIBSection(dc, (BITMAPINFO *)&bi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, dc);
    if (!color)
        return NULL;
    mask = CreateBitmap(n, n, 1, 1, NULL);

    for (y = 0; y < n; y++) {
        for (x = 0; x < n; x++) {
            float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
            float r = 0.24f, half = 0.47f - r;
            float dx = absf(u - 0.5f) - half, dy = absf(v - 0.5f) - half;
            float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
            float in = dx > dy ? dx : dy;
            float sd = sqrtf_(ox * ox + oy * oy) + (in < 0 ? in : 0) - r;
            float a = clampf(0.5f - sd * n, 0, 1);
            float t = clampf((u + v) * 0.5f, 0, 1);
            float cr = 16 + (124 - 16) * t, cg = 150 + (64 - 150) * t, cb = 170 + (214 - 170) * t;
            float d = v - (0.78f - 0.62f * u), band = clampf(1 - absf(d) / 0.11f, 0, 1);
            band *= band;
            cr += (255 - cr) * band * 0.75f;
            cg += (255 - cg) * band * 0.75f;
            cb += (255 - cb) * band * 0.75f;
            ((DWORD *)bits)[y * n + x] = ((DWORD)(a * 255) << 24) | ((DWORD)(cr * a) << 16) |
                                         ((DWORD)(cg * a) << 8) | (DWORD)(cb * a);
        }
    }
    ii.fIcon = TRUE;
    ii.xHotspot = ii.yHotspot = 0;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    icon = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return icon;
}

/* uxtheme ordinal 135 is SetPreferredAppMode (Windows 10 1903+). It is
 * undocumented but stable, and the only way to get dark popup menus. */
void allow_dark_menus(void)
{
    typedef int(WINAPI * SetModeFn)(int);
    typedef void(WINAPI * FlushFn)(void);
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    SetModeFn set;
    FlushFn flush;
    if (!ux)
        return;
    set = (SetModeFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(135));
    flush = (FlushFn)(void *)GetProcAddress(ux, MAKEINTRESOURCEA(136));
    if (set)
        set(1); /* AllowDark: follow the system setting */
    if (flush)
        flush();
}
