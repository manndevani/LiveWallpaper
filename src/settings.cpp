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
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windowsx.h>

namespace {

/* ---- layout (DIPs) ----------------------------------------------------- */

const float W = 780, H = 500;
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
};

const UINT kFps[] = {15, 30, 60};
const WCHAR *const kFpsLabel[] = {L"15", L"30", L"60"};
const UINT kQuality[] = {0, 1, 2}; /* config scale: auto, full, half */
const WCHAR *const kQualityLabel[] = {L"Auto", L"Full", L"Half"};
const WCHAR *const kFitLabel[] = {L"Cover", L"Contain", L"Stretch"};
const WCHAR *const kFilters[] = {L"All", L"Shaders", L"Interactive", L"Videos", L"Images"};
const int FILTERS = 5;

#define WM_APP_ADD (WM_APP + 1)
#define WM_APP_THUMB (WM_APP + 2)

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

int current_index()
{
    for (UINT i = 0; i < item_count(); i++)
        if (str_ieq(item_id(i), cfg.current))
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

void draw_grid()
{
    s.grid = R(0, BAR, LEFT_W, H - FOOT);
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

void draw_inspector()
{
    float x0 = LEFT_W, x = LEFT_W + 16, w = SIDE - 32;
    fill(R(x0, 0, W, H - FOOT), s.th.panel, 0);
    line(x0 + 0.5f, 0, x0 + 0.5f, H - FOOT, s.th.line);

    int cur = current_index();
    EngineStatus st = s.engine.status;
    bool running = st != ST_NOT_RUNNING;
    float y = 14;

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

void draw_footer()
{
    fill(R(0, H - FOOT, W, H), s.th.panel, 0);
    line(0, H - FOOT + 0.5f, W, H - FOOT + 0.5f, s.th.line);
    WCHAR t[128];
    EngineInfo &e = s.engine;
    if (e.status == ST_NOT_RUNNING)
        str_copy(t, L"engine stopped \x00B7 wallpaper is not running", 128);
    else if (e.status == ST_RUNNING && e.fps)
        wsprintfW(t, L"rendering %u fps at %u\x00D7%u \x00B7 runs in the tray", e.fps, e.w, e.h);
    else
        wsprintfW(t, L"0 fps \x00B7 %s \x00B7 runs in the tray", status_text(e.status));
    text(t, R(14, H - FOOT, W - 140, H), s.f_mono, s.th.sub);

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
        lib_remove(&cfg, i);
        apply(false);
    } else if (id >= ID_CARD && id < ID_CARD + (int)item_count()) {
        str_copy(cfg.current, item_id((UINT)(id - ID_CARD)), MAX_PATH);
        apply(true);
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
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

extern "C" void settings_run(BOOL add)
{
    HWND existing = FindWindowW(SETTINGS_CLASS, nullptr);
    if (existing) {
        if (IsIconic(existing))
            ShowWindow(existing, SW_RESTORE);
        SetForegroundWindow(existing);
        if (add)
            PostMessageW(existing, WM_APP_ADD, 0, 0);
        return;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    allow_dark_menus();
    config_load(&cfg);
    s.autostart = autostart_query() != FALSE;
    s.engine = engine_query();

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
    if (add)
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
