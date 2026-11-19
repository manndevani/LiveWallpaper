/* src_image.c - still images and animated GIFs through WIC.
 *
 * Still images are decoded once (scaled down to what the screen can show),
 * uploaded to the GPU, and the CPU copy is freed: zero cost afterwards.
 * GIFs are decoded one frame at a time into a small canvas and uploaded,
 * following the GIF disposal rules, so even long GIFs use little memory. */
#include "common.h"
#include "config.h"
#include "source.h"
#include <wincodec.h>
#include "ps_blit.h"

#define MAX_DIM 8192

static struct {
    IWICImagingFactory *wic;
    IWICBitmapDecoder *dec;
    ID3D11Texture2D *tex;
    ID3D11ShaderResourceView *srv;
    ID3D11PixelShader *ps;
    UINT w, h;         /* canvas / texture size */
    BOOL gif;
    UINT frames, next; /* GIF: frame count, next frame to show */
    BYTE *canvas, *saved, *tmp;
    SIZE_T tmp_size;
    UINT disposal;     /* how to clear the previous frame */
    RECT prev;         /* where the previous frame was */
    double due;        /* animation time the next GIF frame is due, <0 = not started */
} im;

static UINT meta_uint(IWICMetadataQueryReader *r, const WCHAR *name, UINT def)
{
    PROPVARIANT v;
    UINT out = def;
    if (!r)
        return def;
    PropVariantInit(&v);
    if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(r, name, &v))) {
        if (v.vt == VT_UI1) out = v.bVal;
        else if (v.vt == VT_UI2) out = v.uiVal;
        else if (v.vt == VT_UI4) out = v.ulVal;
        PropVariantClear(&v);
    }
    return out;
}

static BOOL to_pbgra(IWICBitmapSource *s, UINT w, UINT h, BYTE *dst)
{
    IWICFormatConverter *c = NULL;
    HRESULT hr = IWICImagingFactory_CreateFormatConverter(im.wic, &c);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_Initialize(c, s, &GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                                            WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr))
        hr = IWICFormatConverter_CopyPixels(c, NULL, w * 4, w * h * 4, dst);
    RELEASE(c);
    return SUCCEEDED(hr);
}

static BOOL make_texture(const BYTE *init)
{
    D3D11_TEXTURE2D_DESC d;
    D3D11_SUBRESOURCE_DATA sd;
    memset(&d, 0, sizeof d);
    d.Width = im.w;
    d.Height = im.h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = init ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = init ? 0 : D3D11_CPU_ACCESS_WRITE;
    sd.pSysMem = init;
    sd.SysMemPitch = im.w * 4;
    sd.SysMemSlicePitch = 0;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(gfx.dev, &d, init ? &sd : NULL, &im.tex)) &&
           SUCCEEDED(ID3D11Device_CreateShaderResourceView(gfx.dev, (ID3D11Resource *)im.tex, NULL, &im.srv));
}

static void im_close(void)
{
    RELEASE(im.srv);
    RELEASE(im.tex);
    RELEASE(im.ps);
    RELEASE(im.dec);
    RELEASE(im.wic);
    mem_free(im.canvas);
    mem_free(im.saved);
    mem_free(im.tmp);
    memset(&im, 0, sizeof im);
}

/* ---- still images ------------------------------------------------------ */

static BOOL still_open(IWICBitmapFrameDecode *frame, UINT w, UINT h)
{
    IWICBitmapScaler *sc = NULL;
    IWICBitmapSource *src = (IWICBitmapSource *)frame;
    float sw = (float)GetSystemMetrics(SM_CXVIRTUALSCREEN), sh = (float)GetSystemMetrics(SM_CYVIRTUALSCREEN);
    float s = sw / w > sh / h ? sw / w : sh / h; /* scale that covers the screen */
    BYTE *px;
    BOOL ok;

    if (s < 1.0f || w > MAX_DIM || h > MAX_DIM) {
        UINT nw, nh;
        if (s > 1.0f)
            s = 1.0f;
        if (w * s > MAX_DIM) s = (float)MAX_DIM / w;
        if (h * s > MAX_DIM) s = (float)MAX_DIM / h;
        nw = max(1, (UINT)(w * s + 0.5f));
        nh = max(1, (UINT)(h * s + 0.5f));
        if (SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(im.wic, &sc)) &&
            SUCCEEDED(IWICBitmapScaler_Initialize(sc, src, nw, nh, WICBitmapInterpolationModeFant))) {
            src = (IWICBitmapSource *)sc;
            w = nw;
            h = nh;
        }
    }
    if (w > MAX_DIM || h > MAX_DIM) {
        RELEASE(sc);
        return FALSE;
    }
    im.w = w;
    im.h = h;
    px = (BYTE *)mem_alloc((SIZE_T)w * h * 4);
    ok = px && to_pbgra(src, w, h, px) && make_texture(px);
    mem_free(px); /* the GPU has its copy now */
    RELEASE(sc);
    return ok;
}

/* ---- animated GIFs ----------------------------------------------------- */

static BOOL gif_open(UINT w, UINT h)
{
    IWICMetadataQueryReader *r = NULL;
    SIZE_T size;
    im.w = w;
    im.h = h;
    if (SUCCEEDED(IWICBitmapDecoder_GetMetadataQueryReader(im.dec, &r))) {
        UINT lw = meta_uint(r, L"/logscrdesc/Width", 0), lh = meta_uint(r, L"/logscrdesc/Height", 0);
        if (lw && lh) {
            im.w = lw;
            im.h = lh;
        }
        RELEASE(r);
    }
    if (im.w > 4096 || im.h > 4096)
        return FALSE;
    size = (SIZE_T)im.w * im.h * 4;
    im.canvas = (BYTE *)mem_alloc(size);
    im.saved = (BYTE *)mem_alloc(size);
    im.due = -1;
    return im.canvas && im.saved && make_texture(NULL);
}

static void clear_rect(const RECT *r)
{
    LONG y;
    for (y = r->top; y < r->bottom; y++)
        memset(im.canvas + ((SIZE_T)y * im.w + r->left) * 4, 0, (SIZE_T)(r->right - r->left) * 4);
}

static void upload(void)
{
    D3D11_MAPPED_SUBRESOURCE m;
    UINT y;
    if (FAILED(ID3D11DeviceContext_Map(gfx.ctx, (ID3D11Resource *)im.tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    for (y = 0; y < im.h; y++)
        memcpy((BYTE *)m.pData + (SIZE_T)y * m.RowPitch, im.canvas + (SIZE_T)y * im.w * 4, (SIZE_T)im.w * 4);
    ID3D11DeviceContext_Unmap(gfx.ctx, (ID3D11Resource *)im.tex, 0);
}

/* Composite the next frame onto the canvas. Returns its delay in seconds. */
static double gif_step(void)
{
    IWICBitmapFrameDecode *f = NULL;
    IWICMetadataQueryReader *r = NULL;
    SIZE_T size = (SIZE_T)im.w * im.h * 4, need;
    UINT left, top, fw = 0, fh = 0, delay = 10, disp = 0, x, y;

    if (im.next == 0) { /* loop restart: start from an empty canvas */
        memset(im.canvas, 0, size);
        im.disposal = 0;
    } else if (im.disposal == 2) {
        clear_rect(&im.prev);
    } else if (im.disposal == 3) {
        memcpy(im.canvas, im.saved, size);
    }

    if (SUCCEEDED(IWICBitmapDecoder_GetFrame(im.dec, im.next, &f))) {
        IWICBitmapFrameDecode_GetMetadataQueryReader(f, &r);
        left = meta_uint(r, L"/imgdesc/Left", 0);
        top = meta_uint(r, L"/imgdesc/Top", 0);
        delay = meta_uint(r, L"/grctlext/Delay", 10);
        disp = meta_uint(r, L"/grctlext/Disposal", 0);
        IWICBitmapFrameDecode_GetSize(f, &fw, &fh);
        if (disp == 3)
            memcpy(im.saved, im.canvas, size);

        need = (SIZE_T)fw * fh * 4;
        if (need > im.tmp_size) {
            mem_free(im.tmp);
            im.tmp = (BYTE *)mem_alloc(need);
            im.tmp_size = im.tmp ? need : 0;
        }
        if (im.tmp && to_pbgra((IWICBitmapSource *)f, fw, fh, im.tmp)) {
            for (y = 0; y < fh && top + y < im.h; y++) {
                const DWORD *s = (const DWORD *)(im.tmp + (SIZE_T)y * fw * 4);
                DWORD *d = (DWORD *)(im.canvas + ((SIZE_T)(top + y) * im.w + left) * 4);
                for (x = 0; x < fw && left + x < im.w; x++)
                    if (s[x] >> 24) /* GIF pixels are fully opaque or fully transparent */
                        d[x] = s[x];
            }
        }
        im.prev.left = (LONG)min(left, im.w);
        im.prev.top = (LONG)min(top, im.h);
        im.prev.right = (LONG)min(left + fw, im.w);
        im.prev.bottom = (LONG)min(top + fh, im.h);
        im.disposal = disp;
        RELEASE(r);
        RELEASE(f);
    }
    im.next = (im.next + 1) % im.frames;
    upload();
    if (delay < 2)
        delay = 10; /* like browsers: 0 or 1 means 100 ms */
    return delay / 100.0;
}

/* ---- source ------------------------------------------------------------ */

static BOOL im_open(const WCHAR *path, SourceInfo *info)
{
    IWICBitmapFrameDecode *f0 = NULL;
    GUID fmt;
    UINT w = 0, h = 0;
    BOOL ok = FALSE;
    HRESULT hr;

    memset(&im, 0, sizeof im);
    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                          (void **)&im.wic);
    if (SUCCEEDED(hr))
        hr = IWICImagingFactory_CreateDecoderFromFilename(im.wic, path, NULL, GENERIC_READ,
                                                          WICDecodeMetadataCacheOnDemand, &im.dec);
    if (SUCCEEDED(hr))
        hr = IWICBitmapDecoder_GetFrameCount(im.dec, &im.frames);
    if (SUCCEEDED(hr))
        hr = IWICBitmapDecoder_GetFrame(im.dec, 0, &f0);
    if (SUCCEEDED(hr))
        hr = IWICBitmapFrameDecode_GetSize(f0, &w, &h);
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreatePixelShader(gfx.dev, g_ps_blit, sizeof g_ps_blit, NULL, &im.ps);
    if (SUCCEEDED(hr) && w && h) {
        im.gif = im.frames > 1 && SUCCEEDED(IWICBitmapDecoder_GetContainerFormat(im.dec, &fmt)) &&
                 !memcmp(&fmt, &GUID_ContainerFormatGif, sizeof fmt);
        ok = im.gif ? gif_open(w, h) : still_open(f0, w, h);
    }
    RELEASE(f0);
    if (!ok) {
        log_line(L"image: cannot open %s (0x%08X)", path, hr);
        im_close();
        return FALSE;
    }
    if (!im.gif)
        RELEASE(im.dec); /* stills are fully on the GPU now */
    info->animated = im.gif;
    info->full_res = TRUE;
    return TRUE;
}

static BOOL im_draw(const FrameInfo *f, BOOL force)
{
    FrameConsts c;
    BOOL changed = force;
    if (!im.srv || !gfx.rtv)
        return FALSE;
    if (im.gif) {
        if (im.due < 0)
            im.due = f->time;
        if (f->time >= im.due) {
            double d = gif_step();
            im.due += d;
            if (im.due < f->time)
                im.due = f->time + d; /* fell behind: don't race to catch up */
            changed = TRUE;
        }
    }
    if (!changed)
        return FALSE;
    gfx_consts(&c, f);
    gfx_fit(im.w, im.h, source_fit, c.fit);
    gfx_draw(im.ps, im.srv, &c);
    return TRUE;
}

const Source src_image = {im_open, im_close, im_draw, NULL, NULL};
