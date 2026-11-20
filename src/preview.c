/* preview.c - see preview.h. Swaps an offscreen texture in as gfx's render
 * target, lets the normal source draw into it, and reads it back. */
#include "common.h"
#include "config.h"
#include "source.h"
#include "preview.h"
#include <wincodec.h>

static BOOL own_device;

/* Fractional part, also for negative x. */
static double frac(double x)
{
    double c = x - (double)(LONGLONG)x;
    return c < 0 ? c + 1.0 : c;
}

/* Close to sin(2*pi*x), without the C runtime: two parabolas. */
static float wave(double x)
{
    double p = frac(x);
    return (float)(p < 0.5 ? 16.0 * p * (0.5 - p) : -16.0 * (p - 0.5) * (1.0 - p));
}

/* A pretend cursor for interactive wallpapers: it drifts around the middle
   and clicks every 3 s, so a series of snapshots shows them moving. At the
   thumbnail time (9 s) the last click was 0.6 s ago, in the centre. */
static void fake_mouse(FrameInfo *f, UINT w, UINT h, double at)
{
    double t = at - 8.4;
    double age = frac(t / 3.0) * 3.0;
    double c = t - age;
    f->mouse[0] = w * 0.5f + w * 0.25f * wave(t / 6.0);
    f->mouse[1] = h * 0.5f + h * 0.15f * wave(t / 3.0);
    f->click[0] = w * 0.5f + w * 0.25f * wave(c / 6.0);
    f->click[1] = h * 0.5f + h * 0.15f * wave(c / 3.0);
    f->click_age = (float)age;
    f->mouse_on = 1.0f;
}

BOOL preview_begin(void)
{
    if (gfx.dev)
        return TRUE;
    own_device = gfx_init(FALSE);
    return own_device;
}

void preview_end(void)
{
    if (own_device)
        gfx_free();
    own_device = FALSE;
}

BOOL preview_render(const WCHAR *id, UINT w, UINT h, double at, BYTE *bgra)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_MAPPED_SUBRESOURCE m;
    ID3D11Texture2D *tex = NULL, *staging = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    const Source *s;
    SourceInfo info;
    FrameInfo f;
    BOOL ok = FALSE;
    UINT i, y;

    if (!gfx.dev || gfx.sc) /* never while a live swap chain owns gfx's target */
        return FALSE;
    memset(&d, 0, sizeof d);
    d.Width = w;
    d.Height = h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(ID3D11Device_CreateTexture2D(gfx.dev, &d, NULL, &tex)) ||
        FAILED(ID3D11Device_CreateRenderTargetView(gfx.dev, (ID3D11Resource *)tex, NULL, &rtv)))
        goto done;
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(gfx.dev, &d, NULL, &staging)))
        goto done;

    gfx.backbuf = tex;
    gfx.rtv = rtv;
    gfx.ww = gfx.bw = w;
    gfx.wh = gfx.bh = h;
    gfx.scale = 1;
    gfx_clear();

    memset(&info, 0, sizeof info);
    memset(&f, 0, sizeof f);
    f.time = at;
    f.phase = phase_at(at);
    fake_mouse(&f, w, h, at);

    s = source_for(kind_of(id));
    if (kind_of(id) != KIND_UNKNOWN && s->open(id, &info)) {
        if (s->play)
            s->play(TRUE);
        /* Video needs a moment to decode its first frame. A forced draw
           before that copies black, so wait for a real frame (up to 3 s). */
        BOOL wait_frame = kind_of(id) == KIND_VIDEO;
        for (i = 0; i < 100 && !ok; i++) {
            ok = s->draw(&f, !wait_frame || i == 99);
            if (!ok)
                Sleep(30);
        }
        s->close();
    }
    if (ok) {
        ID3D11DeviceContext_CopyResource(gfx.ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
        ok = SUCCEEDED(ID3D11DeviceContext_Map(gfx.ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
        if (ok) {
            for (y = 0; y < h; y++)
                memcpy(bgra + (SIZE_T)y * w * 4, (BYTE *)m.pData + (SIZE_T)y * m.RowPitch, (SIZE_T)w * 4);
            ID3D11DeviceContext_Unmap(gfx.ctx, (ID3D11Resource *)staging, 0);
        }
    }

done:
    gfx_unbind();
    gfx.backbuf = NULL;
    gfx.rtv = NULL;
    gfx.ww = gfx.wh = gfx.bw = gfx.bh = 0;
    RELEASE(staging);
    RELEASE(rtv);
    RELEASE(tex);
    return ok;
}

BOOL preview_save_png(const WCHAR *path, UINT w, UINT h, const BYTE *bgra)
{
    IWICImagingFactory *wic = NULL;
    IWICStream *stream = NULL;
    IWICBitmapEncoder *enc = NULL;
    IWICBitmapFrameEncode *frame = NULL;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                          (void **)&wic);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(wic, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(stream, path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(wic, &GUID_ContainerFormatPng, NULL, &enc);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(frame, NULL);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(frame, w, h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(frame, &fmt);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WritePixels(frame, h, w * 4, w * h * 4, (BYTE *)bgra);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
    RELEASE(frame);
    RELEASE(enc);
    RELEASE(stream);
    RELEASE(wic);
    return SUCCEEDED(hr);
}

int snapshot_run(const WCHAR *id, const WCHAR *out, double at)
{
    const UINT w = 960, h = 540;
    BYTE *px;
    int code = 1;
    static WCHAR full[MAX_PATH];
    if (kind_of(id) == KIND_UNKNOWN) { /* allow "aurora" for "builtin:aurora" */
        str_copy(full, L"builtin:", MAX_PATH);
        str_copy(full + 8, id, MAX_PATH - 8);
        if (kind_of(full) != KIND_UNKNOWN)
            id = full;
    }
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    px = (BYTE *)mem_alloc((SIZE_T)w * h * 4);
    if (!px || !preview_begin())
        log_line(L"snapshot: no graphics device");
    else if (!preview_render(id, w, h, at, px))
        log_line(L"snapshot: rendering %s failed (kind %d)", id, (int)kind_of(id));
    else if (!preview_save_png(out, w, h, px))
        log_line(L"snapshot: could not write %s", out);
    else
        code = 0;
    preview_end();
    mem_free(px);
    CoUninitialize();
    return code;
}
