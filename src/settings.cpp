/* settings.cpp - the settings window (livewall.exe -settings).
 *
 * A separate, short-lived process, so the wallpaper engine never carries UI
 * code or memory. Custom-drawn with Direct2D + DirectWrite: a library grid
 * with real previews on the left, an inspector on the right, a status bar
 * at the bottom. Follows the Windows light/dark theme and accent color.
 *
 * C++ is used only for COM convenience: no exceptions, RTTI, STL, global
 * constructors or C runtime. All state is plain data. */
#include "common.h"
#include "config.h"
#include "autostart.h"
#include "ipc.h"
#include "preview.h"
#include "settings.h"
#include "lock.h"
#include "gallery.h"
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.storage.h>
#include <windows.system.userprofile.h>

namespace {

/* ---- layout (DIPs) ----------------------------------------------------- */

const float W = 780, H = 540;
const float TABS = 38;          /* Desktop / Lock screen switch above the inspector */
const float FOOT = 30;          /* status bar height */
const float SIDE = 252;         /* inspector width */
const float LEFT_W = W - SIDE;  /* library pane width */
const float BAR = 48;           /* filter bar height */
const float GAP = 10, PADG = 14;
const int COLS = 3;

const UINT THUMB_W = 320, THUMB_H = 200; /* thumbnail render size, pixels */

enum {
    ID_CARD = 1000,   /* + item index */
    ID_REMOVE = 2000, /* + library index */
    ID_ADD = 10,
    ID_FILTER = 20,   /* + filter index */
    ID_PAUSE = 30, ID_ENGINE,
    ID_SPEED = 40, ID_BRIGHT,
    ID_COVERED = 50, ID_BATTERY, ID_AUTOSTART, ID_IGPU,
    ID_FPS = 60,      /* + index */
    ID_QUALITY = 70,  /* + index */
    ID_FIT = 80,      /* + index */
    ID_FOLDER = 90,
    ID_TAB = 100,     /* + 0 desktop, 1 lock screen */
    ID_LOCK_ON = 110, ID_LOCK_SAME, ID_LOCK_SECURE, ID_LOCK_PICTURE, ID_LOCK_PREVIEW, ID_LOCK_HOTKEY, ID_LOCK_GRACE,
    ID_LOCK_WAIT = 120, /* + index */
    ID_GSHARE = 130, ID_GRETRY,
    ID_GCARD = 3000,  /* + gallery index */
};

const WCHAR *const kTabLabel[] = {L"Desktop", L"Lock screen"};
const UINT kWait[] = {1, 5, 10, 15, 30}; /* minutes */
const WCHAR *const kWaitLabel[] = {L"1", L"5", L"10", L"15", L"30"};

const UINT kFps[] = {15, 30, 60};
const WCHAR *const kFpsLabel[] = {L"15", L"30", L"60"};
const UINT kQuality[] = {0, 1, 2}; /* config scale: auto, full, half */
const WCHAR *const kQualityLabel[] = {L"Auto", L"Full", L"Half"};
const WCHAR *const kFitLabel[] = {L"Cover", L"Contain", L"Stretch"};
const WCHAR *const kFilters[] = {L"All", L"Shaders", L"Interactive", L"Videos", L"Images", L"Community"};
const int FILTERS = 6;
const int COMMUNITY = 5; /* the filter that shows the gallery */

#define WM_APP_ADD (WM_APP + 1)
#define WM_APP_THUMB (WM_APP + 2)
#define WM_APP_LOCKTAB (WM_APP + 3)
#define WM_APP_GINDEX (WM_APP + 4)   /* wParam: count or -1, lParam: GalleryItem array (copy it) */
#define WM_APP_GPREVIEW (WM_APP + 5) /* wParam: index, lParam: ok */
#define WM_APP_GINSTALL (WM_APP + 6) /* wParam: index, lParam: ok */

struct Hit {
    D2D1_RECT_F r;
    int id;
};

struct Thumb {
    WCHAR id[MAX_PATH];
    ID2D1Bitmap *bmp;
    ID2D1BitmapBrush *brush;
    bool tried;
};

struct Theme {
    bool dark;
    D2D1_COLOR_F bg, panel, card, hover, line, text, sub, faint, accent, on_accent;
};

Config cfg;

struct State {
    HWND hwnd;
    ID2D1Factory *d2d;
    ID2D1HwndRenderTarget *rt;
    ID2D1SolidColorBrush *brush;
    ID2D1StrokeStyle *dash;
    IDWriteFactory *dw;
    IDWriteTextFormat *f_title, *f_body, *f_small, *f_mono, *f_icon, *f_icon_s;
    IDWriteInlineObject *ellipsis;
    Theme th;
    Hit hits[200];
    int nhits, hover, pressed, drag;
    int filter;
    float scroll, scroll_max;
    D2D1_RECT_F grid;
    D2D1_RECT_F slider[2]; /* track rects for drag math */
    EngineInfo engine;
    bool autostart;
    bool thumb_posted;
    bool trimmed;        /* memory handed back since the last input */
    DWORD last_input;    /* GetTickCount of the last mouse/keyboard input */
    Thumb thumbs[LIB_MAX + 16];
    int nthumbs;
    int tab;             /* 0 desktop, 1 lock screen */
    LockState lock;      /* Windows' screen saver settings */
    bool grace_zero;     /* Windows asks for the password with no 5 s grace period */

    /* Why the chosen custom shader can't be shown (see current_problem). */
    WCHAR err_for[MAX_PATH];
    WCHAR err_text[240];
    bool err_has;

    /* The Community gallery (gallery.c), filled by worker threads. */
    int gstate;          /* 0 not loaded, 1 loading, 2 ready, 3 couldn't reach it */
    GalleryItem gitems[GALLERY_MAX];
    int gcount;
    struct {
        ID2D1Bitmap *bmp;
        ID2D1BitmapBrush *brush;
        bool ready;      /* the preview is downloaded and checked */
        bool tried;      /* loaded into a bitmap (or failed to) */
        bool failed;     /* the last install of this one failed */
    } gview[GALLERY_MAX];
    int ginstalling;     /* index being downloaded, -1 if none */
} s;

/* Like the engine: most of what Direct2D and the driver hold is needed only
   while building the window and its thumbnails. Hand it back to Windows once
   the window has been left alone; anything needed again pages back in
   cheaply on the next paint. */
void trim_memory()
{
    HeapCompact(GetProcessHeap(), 0);
    SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
    s.trimmed = true;
}

void note_input()
{
    s.last_input = GetTickCount();
    s.trimmed = false;
}

void trim_if_quiet() /* from the 1 s timer */
{
    if (!s.trimmed && !s.thumb_posted && GetTickCount() - s.last_input > 3000)
        trim_memory();
}

/* ---- small helpers ----------------------------------------------------- */

D2D1_COLOR_F rgb(UINT v, float a = 1.0f)
{
    D2D1_COLOR_F c = {((v >> 16) & 255) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f, a};
    return c;
}

D2D1_COLOR_F alpha(D2D1_COLOR_F c, float a)
{
    c.a = a;
    return c;
}

D2D1_RECT_F R(float l, float t, float r, float b)
{
    D2D1_RECT_F x = {l, t, r, b};
    return x;
}

bool inside(D2D1_RECT_F r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

UINT item_count() { return g_builtin_count + cfg.lib_count; }
const WCHAR *item_id(UINT i) { return i < g_builtin_count ? g_builtins[i].id : cfg.lib[i - g_builtin_count]; }
bool item_interactive(UINT i) { return i < g_builtin_count && g_builtins[i].interactive; }

void refresh_lock()
{
    lock_query(&s.lock);
    s.grace_zero = lock_grace_ms() == 0;
}

/* The wallpaper the open tab is about. */
const WCHAR *shown_id() { return s.tab == 1 && cfg.lock[0] ? cfg.lock : cfg.current; }

int current_index()
{
    for (UINT i = 0; i < item_count(); i++)
        if (str_ieq(item_id(i), shown_id()))
            return (int)i;
    return -1;
}

bool passes_filter(UINT i)
{
    Kind k = kind_of(item_id(i));
    switch (s.filter) {
    case 1: return (k == KIND_SHADER && !item_interactive(i)) || k == KIND_CUSTOM_SHADER;
    case 2: return item_interactive(i);
    case 3: return k == KIND_VIDEO;
    case 4: return k == KIND_GIF || k == KIND_IMAGE;
    default: return true;
    }
}

const WCHAR *type_label(UINT i)
{
    if (item_interactive(i))
        return L"INTERACTIVE";
    switch (kind_of(item_id(i))) {
    case KIND_VIDEO: return L"VIDEO";
    case KIND_GIF: return L"GIF";
    case KIND_IMAGE: return L"IMAGE";
    case KIND_CUSTOM_SHADER: return L"CUSTOM";
    default: return L"SHADER";
    }
}

const WCHAR *kind_glyph(UINT i)
{
    if (item_interactive(i))
        return L"\xE7C9";
    switch (kind_of(item_id(i))) {
    case KIND_VIDEO: return L"\xE714";
    case KIND_GIF:
    case KIND_IMAGE: return L"\xEB9F";
    case KIND_CUSTOM_SHADER: return L"\xE943";
    default: return L"\xE790";
    }
}

void invalidate() { InvalidateRect(s.hwnd, nullptr, FALSE); }

/* ---- theme ------------------------------------------------------------- */

void load_theme()
{
    DWORD light = 1, size = sizeof light;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
#ifdef LW_TEST_LIGHT /* testing: light theme without changing the Windows setting */
    light = 1;
#endif
    Theme &t = s.th;
    t.dark = light == 0;
    if (t.dark) {
        t.bg = rgb(0x1C1C1C);
        t.panel = rgb(0x232323);
        t.card = rgb(0x2A2A2A);
        t.hover = rgb(0x323232);
        t.line = rgb(0x383838);
        t.text = rgb(0xF2F2F2);
        t.sub = rgb(0xA6A6A6);
        t.faint = rgb(0x6E6E6E);
    } else {
        t.bg = rgb(0xF7F7F7);
        t.panel = rgb(0xEFEFEF);
        t.card = rgb(0xFFFFFF);
        t.hover = rgb(0xF3F3F3);
        t.line = rgb(0xDDDDDD);
        t.text = rgb(0x1A1A1A);
        t.sub = rgb(0x5F5F5F);
        t.faint = rgb(0x9A9A9A);
    }
    /* Windows' own accent palette: lighter shade on dark, darker on light. */
    BYTE pal[32];
    size = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS && size >= 32) {
        const BYTE *p = pal + (t.dark ? 1 : 4) * 4;
        t.accent = rgb((UINT)(p[0] << 16) | (UINT)(p[1] << 8) | p[2]);
    } else {
        DWORD c = 0;
        BOOL opaque;
        DwmGetColorizationColor(&c, &opaque);
        t.accent = rgb(c & 0xFFFFFF);
    }
    t.on_accent = t.dark ? rgb(0x000000) : rgb(0xFFFFFF);

    BOOL dark = t.dark;
    DwmSetWindowAttribute(s.hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
    COLORREF cap = t.dark ? RGB(0x1C, 0x1C, 0x1C) : RGB(0xF7, 0xF7, 0xF7);
    DwmSetWindowAttribute(s.hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &cap, sizeof cap);
}

/* ---- text -------------------------------------------------------------- */

bool has_font(const WCHAR *name)
{
    IDWriteFontCollection *c = nullptr;
    UINT32 index;
    BOOL ok = FALSE;
    if (SUCCEEDED(s.dw->GetSystemFontCollection(&c, FALSE))) {
        c->FindFamilyName(name, &index, &ok);
        c->Release();
    }
    return ok != FALSE;
}

IDWriteTextFormat *make_format(const WCHAR *family, float size, DWRITE_FONT_WEIGHT weight)
{
    IDWriteTextFormat *f = nullptr;
    s.dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"",
                           &f);
    if (f) {
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (!s.ellipsis)
            s.dw->CreateEllipsisTrimmingSign(f, &s.ellipsis);
        DWRITE_TRIMMING trim = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, s.ellipsis);
    }
    return f;
}

void make_fonts()
{
    const WCHAR *ui = has_font(L"Segoe UI Variable Text") ? L"Segoe UI Variable Text" : L"Segoe UI";
    const WCHAR *disp = has_font(L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
    const WCHAR *mono = has_font(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    const WCHAR *icons = has_font(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    s.f_body = make_format(ui, 13, DWRITE_FONT_WEIGHT_NORMAL);
    s.f_title = make_format(disp, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    s.f_small = make_format(ui, 12, DWRITE_FONT_WEIGHT_NORMAL);
    s.f_mono = make_format(mono, 10.5f, DWRITE_FONT_WEIGHT_NORMAL);
    s.f_icon = make_format(icons, 16, DWRITE_FONT_WEIGHT_NORMAL);
    s.f_icon_s = make_format(icons, 11, DWRITE_FONT_WEIGHT_NORMAL);
}

float text_width(const WCHAR *t, IDWriteTextFormat *f)
{
    IDWriteTextLayout *l = nullptr;
    DWRITE_TEXT_METRICS m;
    m.widthIncludingTrailingWhitespace = 0;
    if (SUCCEEDED(s.dw->CreateTextLayout(t, (UINT32)str_len(t), f, 2000, 100, &l))) {
        l->GetMetrics(&m);
        l->Release();
    }
    return m.widthIncludingTrailingWhitespace;
}

/* ---- drawing primitives ------------------------------------------------ */

void color(D2D1_COLOR_F c) { s.brush->SetColor(c); }

void fill(D2D1_RECT_F r, D2D1_COLOR_F c, float rad = 5)
{
    color(c);
    D2D1_ROUNDED_RECT rr = {r, rad, rad};
    s.rt->FillRoundedRectangle(rr, s.brush);
}

void outline(D2D1_RECT_F r, D2D1_COLOR_F c, float rad = 5, float width = 1, ID2D1StrokeStyle *style = nullptr)
{
    color(c);
    float h = width / 2;
    D2D1_ROUNDED_RECT rr = {R(r.left + h, r.top + h, r.right - h, r.bottom - h), rad, rad};
    s.rt->DrawRoundedRectangle(rr, s.brush, width, style);
}

void line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c)
{
    color(c);
    s.rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), s.brush, 1.0f);
}

void dot(float x, float y, float r, D2D1_COLOR_F c)
{
    color(c);
    D2D1_ELLIPSE e = {D2D1::Point2F(x, y), r, r};
    s.rt->FillEllipse(e, s.brush);
}

void text(const WCHAR *t, D2D1_RECT_F r, IDWriteTextFormat *f, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING)
{
    color(c);
    f->SetTextAlignment(a);
    s.rt->DrawText(t, (UINT32)str_len(t), f, r, s.brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void hit(D2D1_RECT_F r, int id)
{
    if (s.nhits < (int)(sizeof s.hits / sizeof s.hits[0])) {
        s.hits[s.nhits].r = r;
        s.hits[s.nhits].id = id;
        s.nhits++;
    }
}

int hit_at(float x, float y)
{
    for (int i = s.nhits - 1; i >= 0; i--) /* last drawn is on top */
        if (inside(s.hits[i].r, x, y))
            return s.hits[i].id;
    return 0;
}

/* ---- widgets ----------------------------------------------------------- */

void button(D2D1_RECT_F r, const WCHAR *glyph, const WCHAR *label, int id, bool primary, bool enabled = true)
{
    bool hov = enabled && s.hover == id;
    D2D1_COLOR_F bg = primary ? s.th.accent : (hov ? s.th.hover : s.th.card);
    D2D1_COLOR_F fg = primary ? s.th.on_accent : (enabled ? s.th.text : s.th.faint);
    fill(r, bg);
    if (!primary)
        outline(r, s.th.line);
    else if (hov)
        fill(r, s.th.dark ? rgb(0xFFFFFF, 0.10f) : rgb(0x000000, 0.10f));
    float gw = glyph ? 18.0f : 0.0f;
    float tw = text_width(label, s.f_small);
    float x = (r.left + r.right - (gw + tw)) / 2;
    if (glyph)
        text(glyph, R(x, r.top, x + 14, r.bottom), s.f_icon_s, fg);
    text(label, R(x + gw, r.top, r.right, r.bottom), s.f_small, fg);
    if (enabled)
        hit(r, id);
}

void toggle(float x, float y, float w, const WCHAR *label, bool on, int id)
{
    D2D1_RECT_F row = R(x, y, x + w, y + 26);
    text(label, R(x, y, x + w - 44, y + 26), s.f_small, s.th.text);
    D2D1_RECT_F sw = R(x + w - 34, y + 5, x + w, y + 21);
    if (on) {
        fill(sw, s.th.accent, 8);
    } else {
        fill(sw, s.th.card, 8);
        outline(sw, s.th.sub, 8);
    }
    float r = s.hover == id ? 5.0f : 4.0f;
    dot(on ? sw.right - 8 : sw.left + 8, y + 13, r, on ? s.th.on_accent : s.th.sub);
    hit(row, id);
}

void segmented(float x, float y, float w, const WCHAR *const *labels, int n, int sel, int base)
{
    D2D1_RECT_F box = R(x, y, x + w, y + 28);
    fill(box, s.th.card);
    outline(box, s.th.line);
    float cw = (w - 4) / n;
    for (int i = 0; i < n; i++) {
        D2D1_RECT_F c = R(x + 2 + i * cw, y + 2, x + 2 + (i + 1) * cw, y + 26);
        if (i == sel)
            fill(c, s.th.accent, 4);
        else if (s.hover == base + i)
            fill(c, s.th.hover, 4);
        text(labels[i], c, s.f_mono, i == sel ? s.th.on_accent : s.th.sub, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(c, base + i);
    }
}

void slider(float x, float y, float w, const WCHAR *label, const WCHAR *value, float t, int id, int slot)
{
    text(label, R(x, y, x + w, y + 18), s.f_small, s.th.sub);
    text(value, R(x, y, x + w, y + 18), s.f_mono, s.th.text, DWRITE_TEXT_ALIGNMENT_TRAILING);
    float ty = y + 28;
    D2D1_RECT_F track = R(x + 6, ty - 2, x + w - 6, ty + 2);
    s.slider[slot] = track;
    fill(track, s.th.line, 2);
    float kx = track.left + (track.right - track.left) * clampf(t, 0, 1);
    fill(R(track.left, ty - 2, kx, ty + 2), s.th.accent, 2);
    bool active = s.hover == id || s.drag == id;
    dot(kx, ty, active ? 7.0f : 6.0f, s.th.dark ? rgb(0xF2F2F2) : rgb(0xFFFFFF));
    color(s.th.line);
    D2D1_ELLIPSE e = {D2D1::Point2F(kx, ty), active ? 7.0f : 6.0f, active ? 7.0f : 6.0f};
    s.rt->DrawEllipse(e, s.brush, 1.0f);
    hit(R(x, y + 18, x + w, y + 38), id);
}

/* ---- thumbnails -------------------------------------------------------- */

void drop_thumbs()
{
    for (int i = 0; i < s.nthumbs; i++) {
        RELEASE(s.thumbs[i].brush);
        RELEASE(s.thumbs[i].bmp);
        s.thumbs[i].tried = false;
    }
    for (int i = 0; i < GALLERY_MAX; i++) {
        RELEASE(s.gview[i].brush);
        RELEASE(s.gview[i].bmp);
        s.gview[i].tried = false;
    }
}

Thumb *thumb_for(const WCHAR *id)
{
    for (int i = 0; i < s.nthumbs; i++)
        if (str_ieq(s.thumbs[i].id, id))
            return &s.thumbs[i];
    if (s.nthumbs >= (int)(sizeof s.thumbs / sizeof s.thumbs[0]))
        return nullptr;
    Thumb *t = &s.thumbs[s.nthumbs++];
    str_copy(t->id, id, MAX_PATH);
    return t;
}

/* Explorer's own thumbnail (cached by Windows) for videos and images. */
ID2D1Bitmap *shell_thumb(const WCHAR *path)
{
    IShellItemImageFactory *f = nullptr;
    HBITMAP hb = nullptr;
    ID2D1Bitmap *bmp = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path, nullptr, IID_PPV_ARGS(&f)))) {
        SIZE size = {(LONG)THUMB_W, (LONG)THUMB_H};
        f->GetImage(size, SIIGBF_BIGGERSIZEOK, &hb);
        f->Release();
    }
    if (!hb)
        return nullptr;
    IWICImagingFactory *wic = nullptr;
    IWICBitmap *wb = nullptr;
    IWICFormatConverter *conv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &wb)) &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)))
        s.rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    RELEASE(conv);
    RELEASE(wb);
    RELEASE(wic);
    DeleteObject(hb);
    return bmp;
}

/* Shaders are rendered by the real engine code, offscreen. */
ID2D1Bitmap *render_thumb(const WCHAR *id)
{
    ID2D1Bitmap *bmp = nullptr;
    BYTE *px = (BYTE *)mem_alloc((SIZE_T)THUMB_W * THUMB_H * 4);
    if (px && preview_begin() && preview_render(id, THUMB_W, THUMB_H, 9.0, px)) {
        D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        s.rt->CreateBitmap(D2D1::SizeU(THUMB_W, THUMB_H), px, THUMB_W * 4, p, &bmp);
    }
    mem_free(px);
    return bmp;
}

/* Builds one missing thumbnail per message so the window stays responsive. */
void build_next_thumb()
{
    s.thumb_posted = false;
    if (!s.rt)
        return;
    for (UINT i = 0; i < item_count(); i++) {
        Thumb *t = thumb_for(item_id(i));
        if (!t || t->tried)
            continue;
        t->tried = true;
        Kind k = kind_of(t->id);
        t->bmp = (k == KIND_SHADER || k == KIND_CUSTOM_SHADER) ? render_thumb(t->id) : shell_thumb(t->id);
        if (t->bmp)
            s.rt->CreateBitmapBrush(t->bmp,
                                    D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                    &t->brush);
        invalidate();
        PostMessageW(s.hwnd, WM_APP_THUMB, 0, 0);
        s.thumb_posted = true;
        return;
    }
    preview_end(); /* all done: free the GPU device until needed again */
}

void draw_thumb(Thumb *t, D2D1_RECT_F r)
{
    D2D1_SIZE_F bs = t->bmp->GetSize();
    float rw = r.right - r.left, rh = r.bottom - r.top;
    float sc = rw / bs.width > rh / bs.height ? rw / bs.width : rh / bs.height; /* cover */
    float ox = r.left + (rw - bs.width * sc) / 2, oy = r.top + (rh - bs.height * sc) / 2;
    t->brush->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc) * D2D1::Matrix3x2F::Translation(ox, oy));
    D2D1_ROUNDED_RECT rr = {r, 4, 4};
    s.rt->FillRoundedRectangle(rr, t->brush);
}

/* ---- panes ------------------------------------------------------------- */

void draw_filters()
{
    float x = PADG, y = 12;
    for (int i = 0; i < FILTERS; i++) {
        float w = text_width(kFilters[i], s.f_small) + 20;
        D2D1_RECT_F r = R(x, y, x + w, y + 24);
        bool sel = s.filter == i;
        if (sel) {
            fill(r, s.th.card);
            outline(r, s.th.line);
        } else if (s.hover == ID_FILTER + i) {
            fill(r, s.th.hover);
        }
        text(kFilters[i], r, s.f_small, sel ? s.th.text : s.th.sub, DWRITE_TEXT_ALIGNMENT_CENTER);
        hit(r, ID_FILTER + i);
        x += w + 4;
    }
    line(0, BAR - 0.5f, LEFT_W, BAR - 0.5f, s.th.line);
}

/* ---- community gallery ------------------------------------------------- */

void para(const WCHAR *t, D2D1_RECT_F r, D2D1_COLOR_F c);
void apply(bool start);

/* Worker: downloads the list, hands a copy to the window, then fetches the
   previews one by one. */
DWORD WINAPI gallery_thread(LPVOID param)
{
    HWND hwnd = (HWND)param;
    GalleryItem *items = (GalleryItem *)mem_alloc(sizeof(GalleryItem) * GALLERY_MAX);
    int n = items ? gallery_fetch(items, GALLERY_MAX) : -1;
    SendMessageW(hwnd, WM_APP_GINDEX, (WPARAM)n, (LPARAM)items); /* waits while the window copies it */
    for (int i = 0; i < n; i++)
        PostMessageW(hwnd, WM_APP_GPREVIEW, (WPARAM)i, gallery_preview(&items[i]));
    mem_free(items);
    return 0;
}

struct InstallJob {
    HWND hwnd;
    int index;
    GalleryItem item;
};

DWORD WINAPI install_thread(LPVOID param)
{
    InstallJob *job = (InstallJob *)param;
    BOOL ok = gallery_install(&job->item);
    PostMessageW(job->hwnd, WM_APP_GINSTALL, (WPARAM)job->index, ok);
    mem_free(job);
    return 0;
}

void gallery_load()
{
    if (s.gstate == 1)
        return;
    s.gstate = 1;
    HANDLE t = CreateThread(nullptr, 64 * 1024, gallery_thread, s.hwnd, 0, nullptr);
    if (t)
        CloseHandle(t);
    else
        s.gstate = 3;
    invalidate();
}

/* Click on a gallery card: download it (unless it already is), then use it. */
void gallery_pick(int i)
{
    if (i < 0 || i >= s.gcount || s.ginstalling >= 0)
        return;
    InstallJob *job = (InstallJob *)mem_alloc(sizeof(InstallJob));
    if (!job)
        return;
    job->hwnd = s.hwnd;
    job->index = i;
    job->item = s.gitems[i];
    s.ginstalling = i;
    s.gview[i].failed = false;
    HANDLE t = CreateThread(nullptr, 64 * 1024, install_thread, job, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {
        mem_free(job);
        s.ginstalling = -1;
    }
    invalidate();
}

void lock_changed();

void gallery_installed(int i, bool ok)
{
    s.ginstalling = -1;
    if (i < 0 || i >= s.gcount)
        return;
    WCHAR path[MAX_PATH];
    gallery_local_path(&s.gitems[i], path);
    log_line(L"gallery: %s %s", s.gitems[i].id, ok ? L"installed" : L"could not be downloaded or did not match");
    if (!ok) {
        s.gview[i].failed = true;
        invalidate();
        return;
    }
    bool have = false;
    for (UINT k = 0; k < cfg.lib_count; k++)
        if (str_ieq(cfg.lib[k], path))
            have = true;
    if (!have && !lib_add(&cfg, path)) {
        MessageBoxW(s.hwnd, L"Your library is full. Remove a wallpaper you don't use, then try again.", L"livewall",
                    MB_ICONINFORMATION);
        return;
    }
    /* A thumbnail tried before the file was here (say, the config already
       named it) failed and would never be tried again: build it afresh. */
    for (int k = 0; k < s.nthumbs; k++)
        if (str_ieq(s.thumbs[k].id, path)) {
            RELEASE(s.thumbs[k].brush);
            RELEASE(s.thumbs[k].bmp);
            s.thumbs[k].tried = false;
        }
    if (s.tab == 1) {
        str_copy(cfg.lock, path, MAX_PATH);
        lock_changed();
    } else {
        str_copy(cfg.current, path, MAX_PATH);
        apply(true);
    }
}

/* A checked preview picture, as a Direct2D bitmap. */
ID2D1Bitmap *load_png(const WCHAR *path)
{
    IWICImagingFactory *wic = nullptr;
    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICFormatConverter *conv = nullptr;
    ID2D1Bitmap *bmp = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &frame)) && SUCCEEDED(wic->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom)))
        s.rt->CreateBitmapFromWicBitmap(conv, nullptr, &bmp);
    RELEASE(conv);
    RELEASE(frame);
    RELEASE(dec);
    RELEASE(wic);
    return bmp;
}

const WCHAR *gallery_kind(const GalleryItem &g)
{
    return str_ieq(g.ext, L".hlsl") ? L"SHADER" : str_ieq(g.ext, L".mp4") ? L"VIDEO" : str_ieq(g.ext, L".gif") ? L"GIF" : L"IMAGE";
}

void draw_community()
{
    float cw = (LEFT_W - PADG * 2 - GAP * (COLS - 1)) / COLS;
    float thumb_h = (cw - 12) * 10 / 16;
    float ch = 6 + thumb_h + 42;
    D2D1_RECT_F g = s.grid;

    if (s.gstate == 0)
        gallery_load();
    if (s.gstate != 2) {
        float cy = (g.top + g.bottom) / 2;
        para(s.gstate == 3 ? L"Couldn't reach the gallery. Check your internet connection and try again."
                           : L"Loading the gallery\x2026",
             R(g.left + 60, cy - 40, g.right - 60, cy), s.th.sub);
        if (s.gstate == 3)
            button(R((g.left + g.right) / 2 - 60, cy + 4, (g.left + g.right) / 2 + 60, cy + 34), L"\xE72C", L"Try again",
                   ID_GRETRY, false);
        return;
    }

    UINT tiles = (UINT)s.gcount + 1; /* + "share your own" */
    UINT rows = (tiles + COLS - 1) / COLS;
    float content = PADG + rows * ch + (rows - 1) * GAP + PADG;
    s.scroll_max = content - (g.bottom - g.top);
    if (s.scroll_max < 0)
        s.scroll_max = 0;
    s.scroll = clampf(s.scroll, 0, s.scroll_max);

    for (UINT k = 0; k < tiles; k++) {
        float x = PADG + (k % COLS) * (cw + GAP);
        float y = g.top + PADG + (k / COLS) * (ch + GAP) - s.scroll;
        if (y + ch < g.top || y > g.bottom)
            continue;
        D2D1_RECT_F card = R(x, y, x + cw, y + ch);
        D2D1_RECT_F clip = R(card.left, card.top > g.top ? card.top : g.top, card.right,
                             card.bottom < g.bottom ? card.bottom : g.bottom);

        if (k == (UINT)s.gcount) { /* share tile */
            bool hov = s.hover == ID_GSHARE;
            fill(card, hov ? s.th.hover : alpha(s.th.card, 0.5f), 7);
            outline(card, hov ? s.th.sub : s.th.line, 7, 1, s.dash);
            float cy = y + ch / 2;
            text(L"\xE72D", R(x, cy - 30, x + cw, cy - 8), s.f_icon, s.th.sub, DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"Share your own", R(x, cy - 6, x + cw, cy + 12), s.f_small, s.th.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"how to add yours", R(x + 4, cy + 12, x + cw - 4, cy + 28), s.f_small, s.th.faint,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(clip, ID_GSHARE);
            continue;
        }

        int i = (int)k;
        const GalleryItem &gi = s.gitems[i];
        WCHAR path[MAX_PATH];
        gallery_local_path(&gi, path);
        bool sel = str_ieq(path, shown_id());
        bool hov = s.hover == ID_GCARD + i;
        bool busy = s.ginstalling == i;

        fill(card, hov || sel ? s.th.card : alpha(s.th.card, 0.6f), 7);
        if (sel) {
            outline(R(card.left - 2, card.top - 2, card.right + 2, card.bottom + 2), alpha(s.th.accent, 0.35f), 9, 2);
            outline(card, s.th.accent, 7, 1.5f);
        } else {
            outline(card, hov ? s.th.sub : s.th.line, 7);
        }

        D2D1_RECT_F tr = R(x + 6, y + 6, x + cw - 6, y + 6 + thumb_h);
        auto &v = s.gview[i];
        if (v.ready && !v.tried) {
            v.tried = true;
            WCHAR png[MAX_PATH];
            gallery_preview_path(&gi, png);
            v.bmp = load_png(png);
            if (v.bmp)
                s.rt->CreateBitmapBrush(v.bmp,
                                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP,
                                                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR),
                                        &v.brush);
        }
        if (v.brush) {
            Thumb t;
            t.bmp = v.bmp;
            t.brush = v.brush;
            draw_thumb(&t, tr);
        } else {
            fill(tr, s.th.dark ? rgb(0x161616) : rgb(0xE9E9E9), 4);
        }
        if (busy) {
            fill(tr, rgb(0x000000, 0.55f), 4);
            text(L"Downloading\x2026", tr, s.f_small, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
        if (sel)
            dot(tr.right - 9, tr.top + 9, 4, rgb(0x3FB950));

        float ty = tr.bottom + 3;
        const WCHAR *kind = gallery_kind(gi);
        float tw = text_width(kind, s.f_mono);
        text(kind, R(x + cw - 86, ty, x + cw - 8, ty + 20), s.f_mono, s.th.faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
        text(gi.name, R(x + 8, ty, x + cw - 14 - tw, ty + 20), s.f_small, sel || hov ? s.th.text : s.th.sub);
        WCHAR by[64];
        if (v.failed)
            str_copy(by, L"Download failed: click to try again", 64);
        else
            wsprintfW(by, L"by %s", gi.author);
        float lw = 0;
        if (gi.likes && !v.failed) { /* likes, right-aligned */
            WCHAR lk[24];
            wsprintfW(lk, L"\x2665 %u", gi.likes);
            lw = text_width(lk, s.f_small) + 6;
            text(lk, R(x + cw - 8 - lw, ty + 18, x + cw - 8, ty + 36), s.f_small, s.th.faint,
                 DWRITE_TEXT_ALIGNMENT_TRAILING);
        }
        text(by, R(x + 8, ty + 18, x + cw - 8 - lw, ty + 36), s.f_small, v.failed ? rgb(0xE5534B) : s.th.faint);
        hit(clip, ID_GCARD + i);
    }

    if (s.scroll_max > 0) {
        float vh = g.bottom - g.top;
        float bar_h = vh * vh / (vh + s.scroll_max);
        float by = g.top + (vh - bar_h) * (s.scroll / s.scroll_max);
        fill(R(LEFT_W - 6, by + 2, LEFT_W - 3, by + bar_h - 2), s.th.line, 1.5f);
    }
}

void draw_grid()
{
    s.grid = R(0, BAR, LEFT_W, H - FOOT);
    if (s.filter == COMMUNITY) {
        s.rt->PushAxisAlignedClip(s.grid, D2D1_ANTIALIAS_MODE_ALIASED);
        draw_community();
        s.rt->PopAxisAlignedClip();
        return;
    }
    float cw = (LEFT_W - PADG * 2 - GAP * (COLS - 1)) / COLS;
    float thumb_h = (cw - 12) * 10 / 16;
    float ch = 6 + thumb_h + 30;
    int cur = current_index();

    /* Visible items plus the "add" tile. */
    UINT order[LIB_MAX + 16];
    UINT n = 0;
    for (UINT i = 0; i < item_count(); i++)
        if (passes_filter(i))
            order[n++] = i;
    UINT tiles = n + 1;
    UINT rows = (tiles + COLS - 1) / COLS;
    float content = PADG + rows * ch + (rows - 1) * GAP + PADG;
    s.scroll_max = content - (s.grid.bottom - s.grid.top);
    if (s.scroll_max < 0)
        s.scroll_max = 0;
    s.scroll = clampf(s.scroll, 0, s.scroll_max);

    s.rt->PushAxisAlignedClip(s.grid, D2D1_ANTIALIAS_MODE_ALIASED);
    bool missing = false;
    for (UINT k = 0; k < tiles; k++) {
        float x = PADG + (k % COLS) * (cw + GAP);
        float y = s.grid.top + PADG + (k / COLS) * (ch + GAP) - s.scroll;
        if (y + ch < s.grid.top || y > s.grid.bottom)
            continue;
        D2D1_RECT_F card = R(x, y, x + cw, y + ch);
        D2D1_RECT_F clip = R(card.left, card.top > s.grid.top ? card.top : s.grid.top, card.right,
                             card.bottom < s.grid.bottom ? card.bottom : s.grid.bottom);

        if (k == n) { /* add tile */
            bool hov = s.hover == ID_ADD;
            fill(card, hov ? s.th.hover : alpha(s.th.card, 0.5f), 7);
            outline(card, hov ? s.th.sub : s.th.line, 7, 1, s.dash);
            float cy = y + ch / 2;
            text(L"\xE710", R(x, cy - 30, x + cw, cy - 8), s.f_icon, s.th.sub, DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"Add wallpaper", R(x, cy - 6, x + cw, cy + 12), s.f_small, s.th.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            text(L"videos \x00B7 GIFs \x00B7 images \x00B7 shaders", R(x + 4, cy + 12, x + cw - 4, cy + 28), s.f_small, s.th.faint,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(clip, ID_ADD);
            continue;
        }

        UINT i = order[k];
        bool sel = (int)i == cur;
        bool lib = i >= g_builtin_count;
        int rid = lib ? ID_REMOVE + (int)(i - g_builtin_count) : -1;
        bool hov = s.hover == ID_CARD + (int)i || (lib && s.hover == rid);

        fill(card, hov || sel ? s.th.card : alpha(s.th.card, 0.6f), 7);
        if (sel) {
            outline(R(card.left - 2, card.top - 2, card.right + 2, card.bottom + 2), alpha(s.th.accent, 0.35f), 9, 2);
            outline(card, s.th.accent, 7, 1.5f);
        } else {
            outline(card, hov ? s.th.sub : s.th.line, 7);
        }

        D2D1_RECT_F tr = R(x + 6, y + 6, x + cw - 6, y + 6 + thumb_h);
        Thumb *t = thumb_for(item_id(i));
        if (t && t->brush) {
            draw_thumb(t, tr);
        } else {
            fill(tr, s.th.dark ? rgb(0x161616) : rgb(0xE9E9E9), 4);
            text(kind_glyph(i), tr, s.f_icon, s.th.faint, DWRITE_TEXT_ALIGNMENT_CENTER);
            if (t && !t->tried)
                missing = true;
        }
        if (sel)
            dot(tr.right - 9, tr.top + 9, 4, rgb(0x3FB950));

        float ty = tr.bottom + 4;
        text(type_label(i), R(x + cw - 86, ty, x + cw - 8, ty + 22), s.f_mono, s.th.faint,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
        float tw = text_width(type_label(i), s.f_mono);
        text(display_name(item_id(i)), R(x + 8, ty, x + cw - 14 - tw, ty + 22), s.f_small,
             sel || hov ? s.th.text : s.th.sub);
        hit(clip, ID_CARD + (int)i);

        if (lib && hov) { /* remove button on hover */
            D2D1_RECT_F xr = R(tr.left + 5, tr.top + 5, tr.left + 25, tr.top + 25);
            fill(xr, s.hover == rid ? rgb(0xC42B1C) : rgb(0x000000, 0.55f), 10);
            text(L"\xE711", xr, s.f_icon_s, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(xr, rid);
        }
    }
    s.rt->PopAxisAlignedClip();

    if (s.scroll_max > 0) { /* thin scrollbar */
        float vh = s.grid.bottom - s.grid.top;
        float bar_h = vh * vh / (vh + s.scroll_max);
        float by = s.grid.top + (vh - bar_h) * (s.scroll / s.scroll_max);
        fill(R(LEFT_W - 6, by + 2, LEFT_W - 3, by + bar_h - 2), s.th.line, 1.5f);
    }
    if (missing && !s.thumb_posted) {
        s.thumb_posted = true;
        PostMessageW(s.hwnd, WM_APP_THUMB, 0, 0);
    }
}

/* Wrapped text, top-aligned. */
void para(const WCHAR *t, D2D1_RECT_F r, D2D1_COLOR_F c)
{
    IDWriteTextFormat *f = s.f_small;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    text(t, r, f, c);
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void draw_lock(float x, float y, float w)
{
    LockState &l = s.lock;
    text(display_name(shown_id()), R(x, y, x + w, y + 24), s.f_title, s.th.text);
    y += 26;
    text(L"Click a wallpaper to choose it", R(x, y, x + w, y + 18), s.f_small, s.th.sub);
    y += 26;
    toggle(x, y, w, L"Same as desktop", cfg.lock[0] == 0, ID_LOCK_SAME);
    y += 32;
    line(x, y + 0.5f, x + w, y + 0.5f, s.th.line);
    y += 10;

    if (l.managed) {
        para(L"Your organization controls the screen saver on this PC, so livewall can't turn it on.",
             R(x, y, x + w, y + 60), s.th.sub);
        return;
    }
    toggle(x, y, w, L"Show when idle", l.ours != 0, ID_LOCK_ON);
    y += 30;
    if (l.ours) {
        text(L"start after", R(x, y, x + w, y + 18), s.f_mono, s.th.faint);
        text(L"minutes", R(x, y, x + w, y + 18), s.f_mono, s.th.faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
        y += 20;
        int sel = 4;
        for (int i = 0; i < 5; i++)
            if (l.minutes <= kWait[i]) {
                sel = i;
                break;
            }
        segmented(x, y, w, kWaitLabel, 5, sel, ID_LOCK_WAIT);
        y += 36;
        toggle(x, y, w, L"Ask for password", l.secure != 0, ID_LOCK_SECURE);
        y += 26;
        if (l.secure) {
            toggle(x, y, w, L"Ask for PIN right away", s.grace_zero, ID_LOCK_GRACE);
            y += 26;
        }
        toggle(x, y, w, L"Ctrl+Alt+L shows it and locks", cfg.lock_hotkey != 0, ID_LOCK_HOTKEY);
        y += 26;
    }
    toggle(x, y, w, L"Same picture on lock screen", cfg.lock_picture != 0, ID_LOCK_PICTURE);
    y += 34;
    button(R(x, y, x + w, y + 30), L"\xE768", L"Preview", ID_LOCK_PREVIEW, false);
    y += 42;
    line(x, y + 0.5f, x + w, y + 0.5f, s.th.line);
    y += 10;
    const WCHAR *note = !l.ours && l.other ? L"Turning this on replaces your current screen saver."
                        : !l.ours && cfg.lock_hotkey
                            /* word joiners keep "Ctrl+Alt+L" on one line */
                            ? L"Turn on \"Show when idle\" to use Ctrl\x2060+\x2060" L"Alt\x2060+\x2060L with your "
                              L"wallpaper. Until then, it just locks the PC."
                        : l.ours && !l.secure
                            ? L"Anyone can use the PC when it ends. Turn on \"Ask for password\" to lock it."
                        : l.ours && !s.grace_zero ? nullptr /* the warning below */
                        : s.grace_zero && cfg.grace_before[0]
                            ? L"Windows checks your password, not livewall. Exiting livewall from the tray puts "
                              L"Windows' usual wait back."
                            : L"Windows checks your password, not livewall. When you come back, the normal Windows "
                              L"sign-in screen asks for it.";
    if (note) {
        para(note, R(x, y, x + w, H - FOOT - 8), s.th.sub);
        return;
    }
    static WCHAR warn[200];
    DWORD secs = lock_grace_ms() / 1000;
    wsprintfW(warn, L"For the first %u second%s, a click or key press goes back to the desktop with no PIN. "
                    L"Turn on \"Ask for PIN right away\" to stop that.", secs, secs == 1 ? L"" : L"s");
    para(warn, R(x, y, x + w, H - FOOT - 8), s.th.accent);
}

void draw_inspector()
{
    float x0 = LEFT_W, x = LEFT_W + 16, w = SIDE - 32;
    fill(R(x0, 0, W, H - FOOT), s.th.panel, 0);
    line(x0 + 0.5f, 0, x0 + 0.5f, H - FOOT, s.th.line);

    segmented(x, 14, w, kTabLabel, 2, s.tab, ID_TAB);
    if (s.tab == 1) {
        draw_lock(x, 14 + TABS, w);
        return;
    }

    int cur = current_index();
    EngineStatus st = s.engine.status;
    bool running = st != ST_NOT_RUNNING;
    float y = 14 + TABS;

    /* Status badge + type tag. */
    D2D1_COLOR_F dc = st == ST_RUNNING || st == ST_STILL ? rgb(0x3FB950)
                      : st == ST_ERROR                    ? rgb(0xE5534B)
                      : running                           ? rgb(0xD29922)
                                                          : s.th.faint;
    const WCHAR *stext = status_text(st);
    float bw = text_width(stext, s.f_small) + 26;
    D2D1_RECT_F badge = R(x, y, x + bw, y + 22);
    fill(badge, s.th.card, 4);
    outline(badge, s.th.line, 4);
    dot(x + 10, y + 11, 3.5f, dc);
    text(stext, R(x + 18, y, badge.right, y + 22), s.f_small, s.th.text);
    if (cur >= 0)
        text(type_label((UINT)cur), R(x + w - 80, y, x + w, y + 22), s.f_mono, s.th.faint,
             DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 32;

    /* Title + frame rate. */
    WCHAR fps[32];
    if (running && s.engine.fps)
        wsprintfW(fps, L"%u FPS", s.engine.fps);
    else
        wsprintfW(fps, L"%u FPS cap", cfg.fps);
    float fw = text_width(fps, s.f_mono);
    text(cur >= 0 ? display_name(cfg.current) : L"Nothing selected", R(x, y, x + w - fw - 8, y + 24), s.f_title,
         s.th.text);
    text(fps, R(x + w - fw - 4, y, x + w, y + 24), s.f_mono, s.th.faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 32;

    /* Actions. */
    float half = (w - 8) / 2;
    bool paused = st == ST_PAUSED;
    button(R(x, y, x + half, y + 30), paused ? L"\xE768" : L"\xE769", paused ? L"Resume" : L"Pause", ID_PAUSE, false,
           running && st != ST_STILL);
    button(R(x + half + 8, y, x + w, y + 30), running ? L"\xE71A" : L"\xE768", running ? L"Stop" : L"Start",
           ID_ENGINE, !running);
    y += 40;
    line(x, y + 0.5f, x + w, y + 0.5f, s.th.line);
    y += 12;

    /* Sliders. */
    WCHAR v[32];
    wsprintfW(v, L"%u.%02u\x00D7", cfg.speed / 100, cfg.speed % 100);
    slider(x, y, w, L"Speed", v, (cfg.speed - 25) / 175.0f, ID_SPEED, 0);
    y += 42;
    wsprintfW(v, L"%u%%", cfg.brightness);
    slider(x, y, w, L"Brightness", v, (cfg.brightness - 20) / 80.0f, ID_BRIGHT, 1);
    y += 44;
    line(x, y + 0.5f, x + w, y + 0.5f, s.th.line);
    y += 10;

    /* Toggles. */
    toggle(x, y, w, L"Pause when covered", cfg.pause_covered != 0, ID_COVERED);
    y += 26;
    toggle(x, y, w, L"Pause on battery", cfg.pause_battery != 0, ID_BATTERY);
    y += 26;
    toggle(x, y, w, L"Start with Windows", s.autostart, ID_AUTOSTART);
    y += 26;
    toggle(x, y, w, L"Use integrated GPU", cfg.low_power != 0, ID_IGPU);
    y += 32;
    line(x, y + 0.5f, x + w, y + 0.5f, s.th.line);
    y += 10;

    /* Performance. */
    text(L"PERFORMANCE", R(x, y, x + w, y + 18), s.f_mono, s.th.faint);
    text(L"frame cap", R(x, y, x + w, y + 18), s.f_mono, s.th.faint, DWRITE_TEXT_ALIGNMENT_TRAILING);
    y += 22;
    int fsel = cfg.fps <= 15 ? 0 : cfg.fps <= 30 ? 1 : 2;
    segmented(x, y, w, kFpsLabel, 3, fsel, ID_FPS);
    y += 36;
    Kind k = kind_of(cfg.current);
    if (k == KIND_VIDEO || k == KIND_GIF || k == KIND_IMAGE) {
        text(L"fit to screen", R(x, y, x + w, y + 18), s.f_mono, s.th.faint);
        y += 20;
        segmented(x, y, w, kFitLabel, 3, (int)cfg.fit, ID_FIT);
    } else {
        text(L"shader resolution", R(x, y, x + w, y + 18), s.f_mono, s.th.faint);
        y += 20;
        segmented(x, y, w, kQualityLabel, 3, cfg.scale == 0 ? 0 : cfg.scale == 1 ? 1 : 2, ID_QUALITY);
    }
}

/* Why the chosen custom shader can't be shown, or null. Settings compiles it
   itself, once per file and again when the window is activated, so the
   reason shows even while the engine is paused (it only reports a failure
   while drawing). */
const WCHAR *current_problem()
{
    if (kind_of(cfg.current) != KIND_CUSTOM_SHADER)
        return nullptr;
    if (!str_ieq(s.err_for, cfg.current)) {
        str_copy(s.err_for, cfg.current, MAX_PATH);
        s.err_has = shader_problem(cfg.current, s.err_text, 240) != FALSE;
    }
    return s.err_has ? s.err_text : nullptr;
}

void draw_footer()
{
    fill(R(0, H - FOOT, W, H), s.th.panel, 0);
    line(0, H - FOOT + 0.5f, W, H - FOOT + 0.5f, s.th.line);
    WCHAR t[400];
    EngineInfo &e = s.engine;
    const WCHAR *why = current_problem();
    D2D1_COLOR_F tc = s.th.sub;
    if (why || e.status == ST_ERROR) {
        wsprintfW(t, L"%s can't be shown: %s", display_name(cfg.current),
                  why ? why : L"see livewall.log (Open folder)");
        tc = rgb(0xE5534B);
    } else if (e.status == ST_NOT_RUNNING)
        str_copy(t, L"engine stopped \x00B7 wallpaper is not running", 128);
    else if (e.status == ST_RUNNING && e.fps)
        wsprintfW(t, L"rendering %u fps at %u\x00D7%u \x00B7 runs in the tray", e.fps, e.w, e.h);
    else
        wsprintfW(t, L"0 fps \x00B7 %s \x00B7 runs in the tray", status_text(e.status));
    text(t, R(14, H - FOOT, W - 140, H), s.f_mono, tc);

    const WCHAR *label = L"Open folder";
    float lw = text_width(label, s.f_small);
    D2D1_RECT_F r = R(W - 14 - lw - 20, H - FOOT + 4, W - 10, H - 4);
    if (s.hover == ID_FOLDER)
        fill(r, s.th.hover, 4);
    text(L"\xE838", R(r.left + 4, r.top, r.left + 18, r.bottom), s.f_icon_s, s.th.sub);
    text(label, R(r.left + 20, r.top, r.right, r.bottom), s.f_small, s.th.sub);
    hit(r, ID_FOLDER);
}

/* ---- render target ----------------------------------------------------- */

void drop_target()
{
    drop_thumbs();
    RELEASE(s.brush);
    RELEASE(s.rt);
}

bool create_target()
{
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    float dpi = (float)GetDpiForWindow(s.hwnd);
    D2D1_RENDER_TARGET_PROPERTIES p =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(), dpi, dpi);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp =
        D2D1::HwndRenderTargetProperties(s.hwnd, D2D1::SizeU((UINT32)rc.right, (UINT32)rc.bottom));
    if (FAILED(s.d2d->CreateHwndRenderTarget(p, hp, &s.rt)))
        return false;
    s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    s.rt->CreateSolidColorBrush(s.th.text, &s.brush);
    return true;
}

void paint()
{
    if (!s.rt && !create_target())
        return;
    s.rt->BeginDraw();
    s.rt->Clear(s.th.bg);
    s.nhits = 0;
    draw_filters();
    draw_grid();
    draw_inspector();
    draw_footer();
    if (s.rt->EndDraw() == D2DERR_RECREATE_TARGET)
        drop_target();
}

/* ---- actions ----------------------------------------------------------- */

void refresh_engine()
{
    EngineInfo e = engine_query();
    if (e.status != s.engine.status || e.fps != s.engine.fps || e.w != s.engine.w) {
        s.engine = e;
        invalidate();
    }
}

void apply(bool start)
{
    config_save(&cfg);
    if (engine_window())
        engine_reload();
    else if (start)
        engine_start();
    invalidate();
}

void add_files()
{
    IFileOpenDialog *dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return;
    static const COMDLG_FILTERSPEC types[] = {
        {L"Wallpapers", L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.wmv;*.avi;*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;"
                        L"*.tiff;*.jxr;*.hlsl;*.fx"},
        {L"Videos", L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.wmv;*.avi"},
        {L"Images and GIFs", L"*.gif;*.png;*.jpg;*.jpeg;*.bmp;*.webp;*.tif;*.tiff;*.jxr"},
        {L"Shaders (HLSL)", L"*.hlsl;*.fx"},
    };
    FILEOPENDIALOGOPTIONS o = 0;
    dlg->SetFileTypes(4, types);
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    dlg->SetTitle(L"Add wallpapers");
    if (SUCCEEDED(dlg->Show(s.hwnd))) {
        IShellItemArray *items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            bool any = false;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; i++) {
                IShellItem *it = nullptr;
                PWSTR p = nullptr;
                if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                    if (lib_add(&cfg, p) && !any) {
                        str_copy(cfg.current, p, MAX_PATH); /* show the first one right away */
                        any = true;
                    }
                    CoTaskMemFree(p);
                }
                RELEASE(it);
            }
            RELEASE(items);
            if (any) {
                s.filter = 0;
                s.scroll = 1e6f; /* scroll to the end, where new items are */
                apply(true);
            }
        }
    }
    RELEASE(dlg);
}

/* ---- lock screen ------------------------------------------------------- */

/* Waits for a WinRT async call, keeping the window painted. */
bool wait_async(IUnknown *op)
{
    ABI::Windows::Foundation::IAsyncInfo *info = nullptr;
    ABI::Windows::Foundation::AsyncStatus st = ABI::Windows::Foundation::AsyncStatus::Started;
    if (FAILED(op->QueryInterface(IID_PPV_ARGS(&info))))
        return false;
    DWORD t0 = GetTickCount();
    while (SUCCEEDED(info->get_Status(&st)) && st == ABI::Windows::Foundation::AsyncStatus::Started &&
           GetTickCount() - t0 < 15000) {
        MSG m;
        while (PeekMessageW(&m, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE))
            DispatchMessageW(&m);
        Sleep(15);
    }
    info->Release();
    return st == ABI::Windows::Foundation::AsyncStatus::Completed;
}

template <typename T> HRESULT factory(const WCHAR *cls, T **out)
{
    HSTRING_HEADER h;
    HSTRING name;
    HRESULT hr = WindowsCreateStringReference(cls, (UINT32)str_len(cls), &h, &name);
    return SUCCEEDED(hr) ? RoGetActivationFactory(name, __uuidof(T), (void **)out) : hr;
}

/* Hands a PNG to Windows as the lock screen picture. */
bool set_lock_image(const WCHAR *png)
{
    using namespace ABI::Windows::Storage;
    using namespace ABI::Windows::System::UserProfile;
    IStorageFileStatics *files = nullptr;
    ILockScreenStatics *lock = nullptr;
    ABI::Windows::Foundation::IAsyncOperation<StorageFile *> *get = nullptr;
    ABI::Windows::Foundation::IAsyncAction *set = nullptr;
    IStorageFile *file = nullptr;
    HSTRING_HEADER h;
    HSTRING path;
    bool ok = false;
    if (SUCCEEDED(factory(RuntimeClass_Windows_Storage_StorageFile, &files)) &&
        SUCCEEDED(factory(RuntimeClass_Windows_System_UserProfile_LockScreen, &lock)) &&
        SUCCEEDED(WindowsCreateStringReference(png, (UINT32)str_len(png), &h, &path)) &&
        SUCCEEDED(files->GetFileFromPathAsync(path, &get)) && wait_async(get) &&
        SUCCEEDED(get->GetResults(&file)) && SUCCEEDED(lock->SetImageFileAsync(file, &set)))
        ok = wait_async(set);
    RELEASE(set);
    RELEASE(file);
    RELEASE(get);
    RELEASE(lock);
    RELEASE(files);
    return ok;
}

/* Renders the lock screen wallpaper at the screen's size and makes it the
   Windows lock screen picture, so the screen saver and the sign-in screen
   look alike. */
void update_lock_picture()
{
    UINT w = (UINT)GetSystemMetrics(SM_CXSCREEN), h = (UINT)GetSystemMetrics(SM_CYSCREEN);
    if (w > 3840 || h > 2160) { /* plenty for a still picture */
        h = h * 3840 / w;
        w = 3840;
    }
    if (!w || !h)
        return;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514) /* IDC_WAIT */));
    WCHAR png[MAX_PATH];
    app_dir(png);
    str_cat(png, L"\\lockscreen.png", MAX_PATH);
    BYTE *px = (BYTE *)mem_alloc((SIZE_T)w * h * 4);
    bool ok = px && preview_begin() && preview_render(cfg.lock[0] ? cfg.lock : cfg.current, w, h, 8.0, px) &&
              preview_save_png(png, w, h, px);
    mem_free(px);
    if (!s.thumb_posted)
        preview_end();
    if (ok)
        ok = set_lock_image(png);
    log_line(L"lock screen picture %s", ok ? L"set" : L"could not be set");
    SetCursor(old);
}

void lock_changed()
{
    config_save(&cfg); /* the engine doesn't use these, so no reload */
    if (cfg.lock_picture)
        update_lock_picture();
    invalidate();
}

void grace_put_back();

void set_lock(BOOL on, UINT minutes, BOOL secure)
{
    if (!lock_apply(on, minutes, secure))
        MessageBoxW(s.hwnd, L"Windows did not accept the screen saver setting.", L"livewall", MB_ICONWARNING);
    if (!on || !secure)
        grace_put_back(); /* "Ask for PIN right away" only matters with both on */
    refresh_lock();
    invalidate();
}

/* "Ask for PIN right away": Windows' screen saver grace period, 0 or back to
   Windows' default (the value removed). It is a machine-wide setting, so
   Windows asks for admin rights (UAC) and reg.exe makes the change; livewall
   itself never runs elevated. Cancelling the prompt changes nothing. */
/* Sets the grace period to `secs` (digits), or removes it ("-" or empty:
   Windows' default). Returns true if the value is now as asked. */
bool grace_write(HWND owner, const WCHAR *secs)
{
    static WCHAR args[300];
    bool remove = !secs[0] || secs[0] == '-';
    if (remove)
        wsprintfW(args, L"delete \"HKLM\\%s\" /v %s /f", GRACE_KEY, GRACE_VALUE);
    else
        wsprintfW(args, L"add \"HKLM\\%s\" /v %s /t REG_SZ /d %s /f", GRACE_KEY, GRACE_VALUE, secs);
    SHELLEXECUTEINFOW ex;
    memset(&ex, 0, sizeof ex);
    ex.cbSize = sizeof ex;
    ex.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    ex.hwnd = owner;
    ex.lpVerb = L"runas";
    ex.lpFile = L"reg.exe";
    ex.lpParameters = args;
    ex.nShow = SW_HIDE;
    if (ShellExecuteExW(&ex) && ex.hProcess) {
        WaitForSingleObject(ex.hProcess, 10000);
        CloseHandle(ex.hProcess);
    }
    int want = 0, now = lock_grace_raw();
    for (const WCHAR *p = secs; *p >= '0' && *p <= '9'; p++)
        want = want * 10 + (*p - '0');
    bool ok = remove ? now < 0 : now == want;
    log_line(L"grace period %s %s: now %d s", remove ? L"removed" : L"set to", remove ? L"" : secs, now);
    return ok;
}

/* The switch. Turning it on remembers what Windows had, so turning it off
   (or exiting livewall, see grace_restore_run) puts that back. */
void set_grace(bool zero)
{
    if (zero) {
        if (!cfg.grace_before[0]) {
            int was = lock_grace_raw();
            if (was < 0)
                str_copy(cfg.grace_before, L"-", 16);
            else
                wsprintfW(cfg.grace_before, L"%d", was);
        }
        if (grace_write(s.hwnd, L"0"))
            config_save(&cfg);
        else
            cfg.grace_before[0] = 0;
    } else if (grace_write(s.hwnd, cfg.grace_before[0] ? cfg.grace_before : L"-")) {
        cfg.grace_before[0] = 0;
        config_save(&cfg);
    }
    refresh_lock();
    invalidate();
}

/* Puts Windows' grace period back if livewall changed it: when the screen
   saver or its password is turned off here. */
void grace_put_back()
{
    if (cfg.grace_before[0] && grace_write(s.hwnd, cfg.grace_before)) {
        cfg.grace_before[0] = 0;
        config_save(&cfg);
    }
}

void set_slider(int id, float px)
{
    D2D1_RECT_F t = s.slider[id == ID_SPEED ? 0 : 1];
    float v = clampf((px - t.left) / (t.right - t.left), 0, 1);
    if (id == ID_SPEED)
        cfg.speed = (UINT)(25 + v * 175 + 0.5f) / 5 * 5; /* 0.25x .. 2.00x in 0.05 steps */
    else
        cfg.brightness = (UINT)(20 + v * 80 + 0.5f);
    invalidate();
}

void activate(int id)
{
    if (id >= ID_REMOVE && id < ID_REMOVE + LIB_MAX) {
        UINT i = (UINT)(id - ID_REMOVE);
        if (str_ieq(cfg.lib[i], cfg.current))
            str_copy(cfg.current, g_builtins[0].id, MAX_PATH);
        if (str_ieq(cfg.lib[i], cfg.lock))
            cfg.lock[0] = 0;
        lib_remove(&cfg, i);
        apply(false);
    } else if (id >= ID_CARD && id < ID_CARD + (int)item_count() && s.tab == 1) {
        str_copy(cfg.lock, item_id((UINT)(id - ID_CARD)), MAX_PATH);
        lock_changed();
    } else if (id >= ID_CARD && id < ID_CARD + (int)item_count()) {
        str_copy(cfg.current, item_id((UINT)(id - ID_CARD)), MAX_PATH);
        apply(true);
        if (!cfg.lock[0] && cfg.lock_picture)
            update_lock_picture();
    } else if (id >= ID_TAB && id < ID_TAB + 2) {
        s.tab = id - ID_TAB;
        refresh_lock();
        invalidate();
    } else if (id == ID_LOCK_SAME) {
        if (cfg.lock[0])
            cfg.lock[0] = 0;
        else
            str_copy(cfg.lock, cfg.current, MAX_PATH);
        lock_changed();
    } else if (id == ID_LOCK_ON) {
        /* Ask for the password by default: a screen saver that unlocks the PC
           for anyone would be a step back from the normal lock. */
        set_lock(!s.lock.ours, s.lock.minutes, s.lock.ours ? s.lock.secure : TRUE);
    } else if (id >= ID_LOCK_WAIT && id < ID_LOCK_WAIT + 5) {
        set_lock(TRUE, kWait[id - ID_LOCK_WAIT], s.lock.secure);
    } else if (id == ID_LOCK_SECURE) {
        set_lock(TRUE, s.lock.minutes, !s.lock.secure);
    } else if (id == ID_LOCK_GRACE) {
        set_grace(!s.grace_zero);
    } else if (id == ID_LOCK_HOTKEY) {
        cfg.lock_hotkey = !cfg.lock_hotkey;
        apply(false); /* the engine owns the shortcut */
    } else if (id == ID_LOCK_PICTURE) {
        cfg.lock_picture = !cfg.lock_picture;
        config_save(&cfg);
        if (cfg.lock_picture)
            update_lock_picture();
        invalidate();
    } else if (id == ID_LOCK_PREVIEW) {
        WCHAR exe[MAX_PATH];
        if (GetModuleFileNameW(nullptr, exe, MAX_PATH))
            ShellExecuteW(s.hwnd, L"open", exe, L"/s", nullptr, SW_SHOWNORMAL);
    } else if (id >= ID_GCARD && id < ID_GCARD + s.gcount) {
        gallery_pick(id - ID_GCARD);
    } else if (id == ID_GRETRY) {
        gallery_load();
    } else if (id == ID_GSHARE) {
        ShellExecuteW(s.hwnd, L"open", GALLERY_SHARE_URL, nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == ID_ADD) {
        add_files();
    } else if (id >= ID_FILTER && id < ID_FILTER + FILTERS) {
        s.filter = id - ID_FILTER;
        s.scroll = 0;
        invalidate();
    } else if (id == ID_PAUSE) {
        engine_pause(s.engine.status == ST_PAUSED ? 0 : 1);
        Sleep(60);
        refresh_engine();
    } else if (id == ID_ENGINE) {
        if (engine_window())
            engine_stop();
        else
            engine_start();
        Sleep(250);
        refresh_engine();
    } else if (id == ID_COVERED) {
        cfg.pause_covered = !cfg.pause_covered;
        apply(false);
    } else if (id == ID_BATTERY) {
        cfg.pause_battery = !cfg.pause_battery;
        apply(false);
    } else if (id == ID_IGPU) {
        cfg.low_power = !cfg.low_power;
        apply(false);
    } else if (id == ID_AUTOSTART) {
        bool on = !s.autostart;
        HCURSOR old = SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32514) /* IDC_WAIT */));
        if (autostart_apply(on)) {
            s.autostart = on;
            cfg.autostart = on;
            if (on)
                autostart_command(cfg.autostart_exe);
        }
        SetCursor(old);
        apply(false);
    } else if (id >= ID_FPS && id < ID_FPS + 3) {
        cfg.fps = kFps[id - ID_FPS];
        apply(false);
    } else if (id >= ID_QUALITY && id < ID_QUALITY + 3) {
        cfg.scale = kQuality[id - ID_QUALITY];
        apply(false);
    } else if (id >= ID_FIT && id < ID_FIT + 3) {
        cfg.fit = (Fit)(id - ID_FIT);
        apply(false);
    } else if (id == ID_FOLDER) {
        WCHAR dir[MAX_PATH];
        app_dir(dir);
        ShellExecuteW(s.hwnd, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
    }
}

/* ---- window ------------------------------------------------------------ */

float dip(int px) { return px * 96.0f / (float)GetDpiForWindow(s.hwnd); }

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (s.rt)
            s.rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        return 0;
    case WM_DPICHANGED: {
        RECT *r = (RECT *)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        if (s.rt)
            s.rt->SetDpi((float)HIWORD(w), (float)HIWORD(w));
        invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
        if (l && !lstrcmpW((LPCWSTR)l, L"ImmersiveColorSet")) {
            load_theme();
            invalidate();
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        load_theme();
        invalidate();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE && !s.drag) { /* pick up changes made from the tray */
            note_input();
            config_load(&cfg);
            refresh_engine();
            refresh_lock();
            s.err_for[0] = 0; /* the shader file may have been edited meanwhile */
            invalidate();
        } else if (LOWORD(w) == WA_INACTIVE && !s.thumb_posted) {
            trim_memory(); /* in the background: give it back right away */
        }
        break;
    case WM_TIMER:
        refresh_engine();
        trim_if_quiet();
        return 0;
    case WM_MOUSEMOVE: {
        note_input();
        float x = dip(GET_X_LPARAM(l)), y = dip(GET_Y_LPARAM(l));
        if (s.drag) {
            set_slider(s.drag, x);
            return 0;
        }
        int id = hit_at(x, y);
        if (id != s.hover) {
            s.hover = id;
            invalidate();
            TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s.hover) {
            s.hover = 0;
            invalidate();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        note_input();
        float x = dip(GET_X_LPARAM(l)), y = dip(GET_Y_LPARAM(l));
        s.pressed = hit_at(x, y);
        SetCapture(h);
        if (s.pressed == ID_SPEED || s.pressed == ID_BRIGHT) {
            s.drag = s.pressed;
            set_slider(s.drag, x);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        float x = dip(GET_X_LPARAM(l)), y = dip(GET_Y_LPARAM(l));
        ReleaseCapture();
        if (s.drag) {
            s.drag = 0;
            apply(false); /* one reload per drag, not per pixel */
        } else {
            int id = hit_at(x, y);
            if (id && id == s.pressed)
                activate(id);
        }
        s.pressed = 0;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        note_input();
        POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        if (inside(s.grid, dip(p.x), dip(p.y))) {
            s.scroll = clampf(s.scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.0f * 60.0f, 0, s.scroll_max);
            invalidate();
        }
        return 0;
    }
    case WM_KEYDOWN:
        note_input();
        if (w == VK_ESCAPE)
            DestroyWindow(h);
        return 0;
    case WM_APP_ADD:
        add_files();
        return 0;
    case WM_APP_THUMB:
        build_next_thumb();
        return 0;
    case WM_APP_LOCKTAB:
        activate(ID_TAB + 1);
        return 0;
    case WM_APP_GINDEX: {
        int n = (int)w;
        if (n >= 0 && l) {
            memcpy(s.gitems, (const void *)l, sizeof(GalleryItem) * (SIZE_T)n);
            s.gcount = n;
            s.gstate = 2;
        } else {
            s.gstate = 3;
        }
        log_line(L"gallery: %d wallpapers listed%s", n < 0 ? 0 : n, n < 0 ? L" (couldn't reach the gallery)" : L"");
        invalidate();
        return 0;
    }
    case WM_APP_GPREVIEW:
        if ((int)w < s.gcount && l) {
            s.gview[w].ready = true;
            invalidate();
        }
        return 0;
    case WM_APP_GINSTALL:
        gallery_installed((int)w, l != 0);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

/* livewall.exe -put-back-grace: started by the engine on Exit from the tray.
   If livewall set Windows' grace period to 0, puts the old value back
   (Windows asks for admin rights). If that is cancelled, config.ini keeps
   the old value, so the next Exit or the switch can still put it back. */
extern "C" void grace_restore_run(void)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    config_load(&cfg);
    if (cfg.grace_before[0] && grace_write(nullptr, cfg.grace_before)) {
        cfg.grace_before[0] = 0;
        config_save(&cfg);
    }
    CoUninitialize();
}

extern "C" void settings_run(int page)
{
    HWND existing = FindWindowW(SETTINGS_CLASS, nullptr);
    if (existing) {
        if (IsIconic(existing))
            ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        if (page == SETTINGS_ADD)
            PostMessageW(existing, WM_APP_ADD, 0, 0);
        else if (page == SETTINGS_LOCK)
            PostMessageW(existing, WM_APP_LOCKTAB, 0, 0);
        return;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    allow_dark_menus();
    config_load(&cfg);
    s.autostart = autostart_query() != FALSE;
    s.engine = engine_query();
    refresh_lock();
    s.tab = page == SETTINGS_LOCK ? 1 : 0;
    s.ginstalling = -1;

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s.d2d)) ||
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&s.dw)))) {
        CoUninitialize();
        return;
    }
    float dashes[] = {4.0f, 3.0f};
    s.d2d->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                         D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10.0f,
                                                         D2D1_DASH_STYLE_CUSTOM, 0.0f),
                             dashes, 2, &s.dash);
    make_fonts();

    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    wc.lpszClassName = SETTINGS_CLASS;
    wc.hIcon = make_app_icon(GetSystemMetrics(SM_CXICON));
    wc.hIconSm = make_app_icon(GetSystemMetrics(SM_CXSMICON));
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    s.hwnd = CreateWindowExW(0, SETTINGS_CLASS, L"livewall", style, CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr,
                             nullptr, inst, nullptr);
    if (!s.hwnd) {
        CoUninitialize();
        return;
    }
    UINT dpi = GetDpiForWindow(s.hwnd);
    RECT r = {0, 0, (LONG)(W * dpi / 96), (LONG)(H * dpi / 96)};
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, dpi);
    MONITORINFO mi;
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(s.hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    SetWindowPos(s.hwnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - ww) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - wh) / 2, ww, wh, SWP_NOZORDER);
    load_theme();
    SetTimer(s.hwnd, 1, 1000, nullptr);
    ShowWindow(s.hwnd, SW_SHOW);
    if (page == SETTINGS_ADD)
        PostMessageW(s.hwnd, WM_APP_ADD, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    drop_target();
    preview_end();
    RELEASE(s.f_title);
    RELEASE(s.f_body);
    RELEASE(s.f_small);
    RELEASE(s.f_mono);
    RELEASE(s.f_icon);
    RELEASE(s.f_icon_s);
    RELEASE(s.ellipsis);
    RELEASE(s.dash);
    RELEASE(s.dw);
    RELEASE(s.d2d);
    CoUninitialize();
}
