/* gfx.h - the Direct3D 11 device, the swap chain on our wallpaper window,
 * and the one fullscreen-triangle draw everything uses. C only. */
#pragma once
#include "common.h"
#include "config.h"
#include <d3d11.h>
#include <dxgi1_2.h>

#define PERIOD_S 120.0 /* built-in shaders loop seamlessly every PERIOD_S seconds */

/* What a source needs to know to draw one frame. */
typedef struct {
    double time;        /* seconds of animation (only advances while running) */
    float phase;        /* 0..2pi, loops every PERIOD_S */
    float mouse[2];     /* cursor, back-buffer pixels */
    float click[2];     /* last click on the desktop, back-buffer pixels */
    float click_age;    /* seconds since that click (large when none) */
    float mouse_on;     /* 1 while the cursor is over the desktop */
} FrameInfo;

/* Mirrors cbuffer Frame in src/shaders/common.hlsli (64 bytes). */
typedef struct {
    float res[2], phase, time;
    float mouse[2], click[2];
    float click_age, mouse_on, dim, pad;
    float fit[4];
} FrameConsts;

typedef struct {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    IDXGIFactory2 *fac;
    IDXGISwapChain1 *sc;
    ID3D11Texture2D *backbuf;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps_dim;
    ID3D11BlendState *blend;
    ID3D11Buffer *cb;
    ID3D11SamplerState *samp;
    UINT ww, wh;     /* window size */
    UINT bw, bh;     /* back buffer size */
    UINT scale;
} Gfx;

extern Gfx gfx;
extern Fit source_fit;     /* how images and video fill the screen */
extern float source_speed; /* playback speed, 1 = normal */

float phase_at(double t);

BOOL gfx_init(BOOL low_power);
void gfx_free(void);
BOOL gfx_attach(HWND wnd, UINT w, UINT h, UINT scale);
void gfx_detach(void);
BOOL gfx_resize(UINT w, UINT h, UINT scale);

void gfx_consts(FrameConsts *c, const FrameInfo *f);
void gfx_fit(UINT cw, UINT ch, Fit fit, float out[4]);
void gfx_draw(ID3D11PixelShader *ps, ID3D11ShaderResourceView *srv, const FrameConsts *c);
void gfx_dim(float amount); /* darken the frame: 0 none, 1 black */
void gfx_clear(void);
void gfx_unbind(void);
HRESULT gfx_present(void);
