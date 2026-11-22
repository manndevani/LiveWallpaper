/* src_video.c - video, decoded in a sandboxed helper process (decoder.c).
 * Anything Windows can play works (MP4/H.264, HEVC, WebM/VP9 and AV1 when
 * their codecs are installed), muted and looped.
 *
 * This side opens the file, starts the helper with that one file handle,
 * and shares a texture with it on the graphics card. Each new frame is one
 * copy on the card into our back buffer; nothing passes through memory, so
 * a 4K video costs what it did when we decoded it ourselves. Media
 * Foundation is never loaded into this process. */
#include "common.h"
#include "config.h"
#include "source.h"
#include "decoder.h"
#include "sandbox.h"
#include <d3d11_1.h>


static Sandboxed helper;
static DecodeShare *sh;
static HANDLE kick;
static ID3D11Texture2D *tex;
static IDXGIKeyedMutex *km;
static UINT tex_w, tex_h;
static BOOL broken;
static WCHAR parked_path[MAX_PATH]; /* see v_park */
static double parked_at;

static const WCHAR *stage(LONG at)
{
    switch (at) {
    case DECODE_AT_DEVICE: return L"opening the graphics card";
    case DECODE_AT_ENGINE: return L"starting Media Foundation";
    case DECODE_AT_SOURCE: return L"opening the file";
    case DECODE_AT_PLAYBACK: return L"playing";
    case DECODE_AT_TEXTURE: return L"opening the shared picture";
    default: return L"starting";
    }
}

static void drop_texture(void)
{
    RELEASE(km);
    RELEASE(tex);
    tex_w = tex_h = 0;
}

static void v_close(void)
{
    if (sh) {
        sh->quit = 1;
        SetEvent(kick);
        if (helper.process)
            WaitForSingleObject(helper.process, 1000); /* let it close cleanly */
    }
    sandbox_end(&helper);
    drop_texture();
    if (sh)
        UnmapViewOfFile(sh);
    sh = NULL;
    if (kick)
        CloseHandle(kick);
    kick = NULL;
    /* Forget the video's buffer size; the engine resizes for whatever comes
       next (and v_draw asks again after a park). */
    gfx.fix_w = gfx.fix_h = 0;
}

static BOOL get_adapter(LUID *out)
{
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC d;
    BOOL ok;
    memset(&d, 0, sizeof d);
    ok = SUCCEEDED(ID3D11Device_QueryInterface(gfx.dev, &IID_IDXGIDevice, (void **)&dd)) &&
              SUCCEEDED(IDXGIDevice_GetAdapter(dd, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d));
    RELEASE(ad);
    RELEASE(dd);
    if (ok)
        *out = d.AdapterLuid;
    return ok;
}

static BOOL v_open(const WCHAR *path, SourceInfo *info)
{
    SECURITY_ATTRIBUTES inh = {sizeof inh, NULL, TRUE};
    HANDLE file, map, self = NULL, pass[4];
    WCHAR args[96];
    BOOL ok;

    broken = FALSE;
    if (path != parked_path) {
        str_copy(parked_path, path, MAX_PATH); /* remembered for v_park */
        parked_at = 0;
    }
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, &inh, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    map = CreateFileMappingW(INVALID_HANDLE_VALUE, &inh, PAGE_READWRITE, 0, sizeof(DecodeShare), NULL);
    kick = CreateEventW(&inh, FALSE, FALSE, NULL);
    DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &self, SYNCHRONIZE, TRUE, 0);
    sh = map ? (DecodeShare *)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DecodeShare)) : NULL;
    ok = sh && kick && self && get_adapter(&sh->adapter);
    if (ok) {
        sh->file = (UINT64)(ULONG_PTR)file;
        str_copy(sh->ext, path_ext(path), 16);
        sh->fit = source_fit;
        sh->speed = source_speed;
        sh->fps_cap = source_fps;
        if (parked_at > 0) { /* back from a park: carry on where it stopped */
            sh->seek_to = parked_at;
            sh->seek_gen = 1;
        }
        wsprintfW(args, L"%s %u %u %u", DECODE_FLAG, (UINT)(ULONG_PTR)map, (UINT)(ULONG_PTR)kick,
                  (UINT)(ULONG_PTR)self);
        pass[0] = map;
        pass[1] = kick;
        pass[2] = self;
        pass[3] = file;
        ok = sandbox_start(args, pass, 4, &helper);
    }
    /* The helper has its own copies now. */
    CloseHandle(file);
    if (map)
        CloseHandle(map);
    if (self)
        CloseHandle(self);
    if (!ok) {
        log_line(L"video: cannot open %s", path);
        v_close();
        return FALSE;
    }
    info->animated = TRUE;
    info->full_res = TRUE;
    info->tick_fps = 60; /* check for new frames often; we only present when one arrives */
    return TRUE;
}

static void v_play(BOOL on)
{
    if (!sh)
        return;
    sh->speed = source_speed;
    sh->fps_cap = source_fps;
    sh->playing = on;
    SetEvent(kick);
}

/* A texture the size of our back buffer, shared with the helper. */
static BOOL make_texture(void)
{
    D3D11_TEXTURE2D_DESC d;
    IDXGIResource1 *res = NULL;
    HANDLE h = NULL, theirs = NULL;
    BOOL ok;

    drop_texture();
    memset(&d, 0, sizeof d);
    d.Width = gfx.bw;
    d.Height = gfx.bh;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE; /* the helper draws into it */
    d.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ok = SUCCEEDED(ID3D11Device_CreateTexture2D(gfx.dev, &d, NULL, &tex)) &&
         SUCCEEDED(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIKeyedMutex, (void **)&km)) &&
         SUCCEEDED(ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIResource1, (void **)&res)) &&
         SUCCEEDED(IDXGIResource1_CreateSharedHandle(res, NULL, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                     NULL, &h)) &&
         DuplicateHandle(GetCurrentProcess(), h, helper.process, &theirs, 0, FALSE,
                         DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE);
    RELEASE(res);
    if (!ok) {
        if (h && !theirs)
            CloseHandle(h);
        drop_texture();
        if (!broken)
            log_line(L"video: cannot share a picture with the helper (0x%08X)", GetLastError());
        broken = TRUE;
        return FALSE;
    }
    tex_w = gfx.bw;
    tex_h = gfx.bh;
    sh->tex = (UINT64)(ULONG_PTR)theirs;
    sh->tex_w = tex_w;
    sh->tex_h = tex_h;
    InterlockedIncrement(&sh->tex_gen);
    SetEvent(kick);
    return TRUE;
}

static BOOL v_draw(const FrameInfo *f, BOOL force)
{
    BOOL asked = FALSE;
    HRESULT hr;
    (void)f;

    if (!sh || !gfx.backbuf || broken)
        return FALSE;
    if (!sh->video_w || !sh->video_h)
        return FALSE; /* not loaded yet */
#if LW_TEST_SANDBOX
    {
        static BOOL told;
        if (!told) {
            told = TRUE;
            log_line(L"sandbox test: read a file %u, list a folder %u, connect %u (0 = it worked)", sh->test[0],
                     sh->test[1], sh->test[2]);
        }
    }
#endif
    /* A video smaller than the screen is drawn at its own size and the
       compositor stretches it (gfx_content_size). */
    if (!gfx_content_size(sh->video_w, sh->video_h) || !gfx.backbuf)
        return FALSE;
    sh->fps_cap = source_fps;
    if (sh->fit != (LONG)source_fit) {
        sh->fit = source_fit;
        InterlockedIncrement(&sh->redraw_gen);
        SetEvent(kick);
        asked = TRUE;
    }
    if (!tex || tex_w != gfx.bw || tex_h != gfx.bh) {
        if (!make_texture())
            return FALSE;
        asked = TRUE;
    }
    /* Key 1: a new frame. When the picture must be drawn now (a resize, a
       new fit, also while paused), wait a moment for the helper's redraw,
       or take the last frame again (key 0). */
    hr = IDXGIKeyedMutex_AcquireSync(km, 1, force && asked ? 250 : 0);
    if (hr != S_OK && force)
        hr = IDXGIKeyedMutex_AcquireSync(km, 0, 0);
    if (hr != S_OK)
        return FALSE;
    /* Same size and format, so a plain copy on the card. (Drawing it with a
       shader instead cost the same: FINDINGS.md.) */
    gfx_unbind();
    ID3D11DeviceContext_CopyResource(gfx.ctx, (ID3D11Resource *)gfx.backbuf, (ID3D11Resource *)tex);
    IDXGIKeyedMutex_ReleaseSync(km, 0);
    return TRUE;
}

static BOOL v_failed(void)
{
    DWORD code = 0;
    if (broken)
        return TRUE;
    if (!sh)
        return FALSE;
    if (sh->state == DECODE_FAILED) {
        log_line(L"video: %s failed while %s (0x%08X)", path_file(parked_path), stage(sh->error_at),
                 (UINT)sh->error);
        broken = TRUE;
    } else if (helper.process && WaitForSingleObject(helper.process, 0) == WAIT_OBJECT_0) {
        GetExitCodeProcess(helper.process, &code);
        log_line(L"video: the helper for %s stopped (0x%08X)", path_file(parked_path), code);
        broken = TRUE;
    }
    return broken;
}

void video_preview_seek(double at)
{
    LONG gen;
    int i;
    for (i = 0; i < 100 && sh && sh->state != DECODE_FAILED && !(sh->length > 0); i++)
        Sleep(30);
    if (!sh || !(sh->length > 0))
        return;
    if (at > sh->length / 3)
        at = sh->length / 3;
    sh->seek_to = at;
    gen = InterlockedIncrement(&sh->seek_gen);
    SetEvent(kick);
    for (i = 0; i < 100 && sh->seek_done != gen && sh->state != DECODE_FAILED; i++)
        Sleep(30);
    /* Throw away a frame drawn before the jump. */
    if (km && IDXGIKeyedMutex_AcquireSync(km, 1, 0) == S_OK)
        IDXGIKeyedMutex_ReleaseSync(km, 0);
}

/* Parking ends the helper (Media Foundation's tens of MB and a dozen
   threads) while the video is paused for a long time. The swap chain keeps
   showing the last frame, so nothing visible changes. */
static void v_park(BOOL park)
{
    SourceInfo info;
    if (park) {
        if (!sh)
            return;
        parked_at = sh->now;
        v_close();
    } else if (!sh && parked_path[0]) {
        memset(&info, 0, sizeof info);
        v_open(parked_path, &info);
    }
}

const Source src_video = {v_open, v_close, v_draw, v_play, v_failed, v_park};
