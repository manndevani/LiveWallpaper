/* gfx.c - see gfx.h. */
#include "common.h"
#include "gfx.h"
#include <dxgi1_6.h>
#include "vs.h"
#include "ps_dim.h"

Gfx gfx;
Fit source_fit = FIT_COVER;
float source_speed = 1.0f;

float phase_at(double t)
{
    double cyc = t / PERIOD_S;
    cyc -= (double)(LONGLONG)cyc;
    return (float)(cyc * 6.283185307179586);
}

/* Pick the GPU that drives the primary monitor, so frames never cross
   between GPUs. -lowpower / "Use integrated GPU" picks the integrated one. */
static IDXGIAdapter1 *pick_adapter(BOOL low_power)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *ad = NULL, *best = NULL;
    IDXGIOutput *out;
    DXGI_OUTPUT_DESC od;
    POINT origin = {0, 0};
    HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    UINT i, j;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)))
        return NULL;
    if (low_power) {
        IDXGIFactory6 *f6 = NULL;
        if (SUCCEEDED(IDXGIFactory1_QueryInterface(f, &IID_IDXGIFactory6, (void **)&f6))) {
            IDXGIFactory6_EnumAdapterByGpuPreference(f6, 0, DXGI_GPU_PREFERENCE_MINIMUM_POWER, &IID_IDXGIAdapter1,
                                                     (void **)&best);
            RELEASE(f6);
        }
    }
    for (i = 0; !best && IDXGIFactory1_EnumAdapters1(f, i, &ad) == S_OK; i++) {
        for (j = 0; !best && IDXGIAdapter1_EnumOutputs(ad, j, &out) == S_OK; j++) {
            if (SUCCEEDED(IDXGIOutput_GetDesc(out, &od)) && od.Monitor == primary) {
                best = ad;
                ad = NULL;
            }
            RELEASE(out);
        }
        RELEASE(ad);
    }
    RELEASE(f);
    return best;
}

BOOL gfx_init(BOOL low_power)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS;
    IDXGIAdapter1 *pick = pick_adapter(low_power);
    D3D_DRIVER_TYPE type = pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
    D3D11_BUFFER_DESC bd;
    D3D11_SAMPLER_DESC sd;
    D3D11_BLEND_DESC bl;
    IDXGIDevice1 *dd = NULL;
    IDXGIAdapter *ad = NULL;
    HRESULT hr;

    hr = D3D11CreateDevice((IDXGIAdapter *)pick, type, NULL, flags, levels, 3, D3D11_SDK_VERSION, &gfx.dev, NULL,
                           &gfx.ctx);
    if (FAILED(hr)) /* no video support on this adapter: shaders and images still work */
        hr = D3D11CreateDevice((IDXGIAdapter *)pick, type, NULL, flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels,
                               3, D3D11_SDK_VERSION, &gfx.dev, NULL, &gfx.ctx);
    RELEASE(pick);
    if (FAILED(hr))
        return FALSE;

    /* Queue at most one frame, and use the factory that owns the device. */
    if (SUCCEEDED(ID3D11Device_QueryInterface(gfx.dev, &IID_IDXGIDevice1, (void **)&dd))) {
        IDXGIDevice1_SetMaximumFrameLatency(dd, 1);
        if (SUCCEEDED(IDXGIDevice1_GetAdapter(dd, &ad)))
            IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory2, (void **)&gfx.fac);
        RELEASE(ad);
        RELEASE(dd);
    }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(FrameConsts);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    memset(&bl, 0, sizeof bl); /* dim pass: dst * (1 - alpha), keep dst alpha */
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    if (!gfx.fac ||
        FAILED(ID3D11Device_CreateVertexShader(gfx.dev, g_vs, sizeof g_vs, NULL, &gfx.vs)) ||
        FAILED(ID3D11Device_CreatePixelShader(gfx.dev, g_ps_dim, sizeof g_ps_dim, NULL, &gfx.ps_dim)) ||
        FAILED(ID3D11Device_CreateBuffer(gfx.dev, &bd, NULL, &gfx.cb)) ||
        FAILED(ID3D11Device_CreateSamplerState(gfx.dev, &sd, &gfx.samp)) ||
        FAILED(ID3D11Device_CreateBlendState(gfx.dev, &bl, &gfx.blend))) {
        gfx_free();
        return FALSE;
    }

    /* This pipeline state never changes, so bind it once. */
    ID3D11DeviceContext_IASetPrimitiveTopology(gfx.ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(gfx.ctx, gfx.vs, NULL, 0);
    ID3D11DeviceContext_PSSetConstantBuffers(gfx.ctx, 0, 1, &gfx.cb);
    ID3D11DeviceContext_PSSetSamplers(gfx.ctx, 0, 1, &gfx.samp);
    return TRUE;
}

void gfx_free(void)
{
    gfx_detach();
    if (gfx.ctx)
        ID3D11DeviceContext_ClearState(gfx.ctx);
    RELEASE(gfx.blend);
    RELEASE(gfx.samp);
    RELEASE(gfx.cb);
    RELEASE(gfx.ps_dim);
    RELEASE(gfx.vs);
    RELEASE(gfx.fac);
    RELEASE(gfx.ctx);
    RELEASE(gfx.dev);
}

static void set_sizes(UINT w, UINT h, UINT scale)
{
    gfx.ww = w;
    gfx.wh = h;
    gfx.scale = scale ? scale : 1;
    gfx.bw = max(1, (w + gfx.scale - 1) / gfx.scale);
    gfx.bh = max(1, (h + gfx.scale - 1) / gfx.scale);
}

static BOOL make_views(void)
{
    if (FAILED(IDXGISwapChain1_GetBuffer(gfx.sc, 0, &IID_ID3D11Texture2D, (void **)&gfx.backbuf)))
        return FALSE;
    return SUCCEEDED(ID3D11Device_CreateRenderTargetView(gfx.dev, (ID3D11Resource *)gfx.backbuf, NULL, &gfx.rtv));
}

BOOL gfx_attach(HWND wnd, UINT w, UINT h, UINT scale)
{
    DXGI_SWAP_CHAIN_DESC1 d;
    set_sizes(w, h, scale);
    memset(&d, 0, sizeof d);
    d.Width = gfx.bw;
    d.Height = gfx.bh;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.BufferCount = 2;
    d.Scaling = DXGI_SCALING_STRETCH; /* the compositor upscales reduced-resolution shaders for free */
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    if (FAILED(IDXGIFactory2_CreateSwapChainForHwnd(gfx.fac, (IUnknown *)gfx.dev, wnd, &d, NULL, NULL, &gfx.sc)) ||
        !make_views()) {
        gfx_detach();
        return FALSE;
    }
    IDXGIFactory2_MakeWindowAssociation(gfx.fac, wnd, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
    return TRUE;
}

void gfx_unbind(void)
{
    if (gfx.ctx)
        ID3D11DeviceContext_OMSetRenderTargets(gfx.ctx, 0, NULL, NULL);
}

void gfx_detach(void)
{
    gfx_unbind();
    if (gfx.ctx)
        ID3D11DeviceContext_Flush(gfx.ctx);
    if (gfx.sc) { /* views we created for a swap chain; preview targets clean up themselves */
        RELEASE(gfx.rtv);
        RELEASE(gfx.backbuf);
        RELEASE(gfx.sc);
    }
}

BOOL gfx_resize(UINT w, UINT h, UINT scale)
{
    if (!gfx.sc)
        return FALSE;
    if (w == gfx.ww && h == gfx.wh && (scale ? scale : 1) == gfx.scale)
        return TRUE;
    gfx_unbind();
    RELEASE(gfx.rtv);
    RELEASE(gfx.backbuf);
    set_sizes(w, h, scale);
    return SUCCEEDED(IDXGISwapChain1_ResizeBuffers(gfx.sc, 0, gfx.bw, gfx.bh, DXGI_FORMAT_UNKNOWN, 0)) &&
           make_views();
}

void gfx_consts(FrameConsts *c, const FrameInfo *f)
{
    double t = f->time;
    memset(c, 0, sizeof *c);
    t -= (double)(LONGLONG)(t / 86400.0) * 86400.0; /* keep float precision: wrap daily */
    c->res[0] = (float)gfx.bw;
    c->res[1] = (float)gfx.bh;
    c->phase = f->phase;
    c->time = (float)t;
    c->mouse[0] = f->mouse[0];
    c->mouse[1] = f->mouse[1];
    c->click[0] = f->click[0];
    c->click[1] = f->click[1];
    c->click_age = f->click_age;
    c->mouse_on = f->mouse_on;
    c->fit[0] = c->fit[1] = 1.0f;
}

/* UV scale/offset so content of size cw x ch covers, fits inside, or is
   stretched over the back buffer. Contain leaves uv outside 0..1 (black). */
void gfx_fit(UINT cw, UINT ch, Fit fit, float o[4])
{
    float tw = (float)gfx.bw, th = (float)gfx.bh, sx, sy, s;
    o[0] = o[1] = 1.0f;
    o[2] = o[3] = 0.0f;
    if (fit == FIT_STRETCH || !cw || !ch)
        return;
    sx = tw / (float)cw;
    sy = th / (float)ch;
    s = fit == FIT_COVER ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);
    o[0] = tw / ((float)cw * s);
    o[1] = th / ((float)ch * s);
    o[2] = (1.0f - o[0]) * 0.5f;
    o[3] = (1.0f - o[1]) * 0.5f;
}

void gfx_draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, const FrameConsts *c)
{
    D3D11_VIEWPORT vp;
    vp.TopLeftX = 0;
    vp.TopLeftY = 0;
    vp.Width = (float)gfx.bw;
    vp.Height = (float)gfx.bh;
    vp.MinDepth = 0;
    vp.MaxDepth = 1;
    ID3D11DeviceContext_UpdateSubresource(gfx.ctx, (ID3D11Resource *)gfx.cb, 0, NULL, c, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(gfx.ctx, 1, &gfx.rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(gfx.ctx, 1, &vp);
    ID3D11DeviceContext_PSSetShader(gfx.ctx, ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(gfx.ctx, 0, 1, &srv);
    ID3D11DeviceContext_Draw(gfx.ctx, 3, 0);
}

void gfx_dim(float amount)
{
    static const float factor[4] = {0, 0, 0, 0};
    FrameConsts c;
    if (amount <= 0.002f || !gfx.rtv)
        return;
    memset(&c, 0, sizeof c);
    c.res[0] = (float)gfx.bw;
    c.res[1] = (float)gfx.bh;
    c.dim = amount > 1.0f ? 1.0f : amount;
    ID3D11DeviceContext_OMSetBlendState(gfx.ctx, gfx.blend, factor, 0xFFFFFFFF);
    gfx_draw(gfx.ps_dim, NULL, &c);
    ID3D11DeviceContext_OMSetBlendState(gfx.ctx, NULL, factor, 0xFFFFFFFF);
}

void gfx_clear(void)
{
    static const float black[4] = {0, 0, 0, 1};
    if (gfx.rtv)
        ID3D11DeviceContext_ClearRenderTargetView(gfx.ctx, gfx.rtv, black);
}

HRESULT gfx_present(void) { return gfx.sc ? IDXGISwapChain1_Present(gfx.sc, 0, 0) : E_FAIL; }
