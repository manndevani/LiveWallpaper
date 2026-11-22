/* src_video.c - video through the Media Foundation media engine.
 * Decoding happens on the GPU (DXVA) and frames are copied straight into
 * our back buffer, so video frames never pass through system memory.
 * Muted and looped. Anything Windows can play works (MP4/H.264, HEVC,
 * WebM/VP9 and AV1 when their codecs are installed). */
#include "common.h"
#include "config.h"
#include "source.h"
#include <mfapi.h>
#include <mfmediaengine.h>

/* Real-time (low-latency) mode keeps fewer decoded frames queued: 36 -> 14 MB
   for the same smooth playback (FINDINGS.md). Overridable with -DLW_ME_FLAGS=
   for measurements. */
#ifndef LW_ME_FLAGS
#define LW_ME_FLAGS MF_MEDIA_ENGINE_REAL_TIME_MODE
#endif

static IMFMediaEngine *eng;
static IMFDXGIDeviceManager *mgr;
static volatile LONG broken;
static BOOL started;
static WCHAR parked_path[MAX_PATH]; /* see v_park */
static double parked_at;
static LONGLONG shown_at; /* when the last frame was shown (QPC), for the frame cap */
static LARGE_INTEGER qpf;

/* ---- the one callback interface Media Foundation needs, by hand in C --- */

static HRESULT STDMETHODCALLTYPE n_qi(IMFMediaEngineNotify *t, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IMFMediaEngineNotify)) {
        *o = t;
        return S_OK;
    }
    *o = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE n_ref(IMFMediaEngineNotify *t)
{
    (void)t;
    return 1; /* static object */
}

/* Called on a Media Foundation worker thread: only set a flag here. */
static HRESULT STDMETHODCALLTYPE n_event(IMFMediaEngineNotify *t, DWORD ev, DWORD_PTR p1, DWORD p2)
{
    (void)t;
    (void)p1;
    (void)p2;
    if (ev == MF_MEDIA_ENGINE_EVENT_ERROR)
        InterlockedExchange(&broken, 1);
    return S_OK;
}

static IMFMediaEngineNotifyVtbl n_vtbl = {n_qi, n_ref, n_ref, n_event};
static IMFMediaEngineNotify notify = {&n_vtbl};

/* ---- source ------------------------------------------------------------ */

static void v_close(void)
{
    if (eng) {
        IMFMediaEngine_Shutdown(eng);
        RELEASE(eng);
    }
    RELEASE(mgr);
    if (started)
        MFShutdown();
    started = FALSE;
    shown_at = 0;
    /* Forget the video's buffer size; the engine resizes for whatever comes
       next (and v_draw asks again after a park). */
    gfx.fix_w = gfx.fix_h = 0;
}

static BOOL v_open(const WCHAR *path, SourceInfo *info)
{
    IMFAttributes *attr = NULL;
    IMFMediaEngineClassFactory *cf = NULL;
    ID3D10Multithread *mt = NULL;
    UINT token = 0;
    BSTR url;
    HRESULT hr;

    broken = 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    if (path != parked_path) {
        str_copy(parked_path, path, MAX_PATH); /* remembered for v_park */
        parked_at = 0;
    }
    hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
        return FALSE;
    started = TRUE;

    /* The media engine uses our device from its own threads. */
    if (SUCCEEDED(ID3D11Device_QueryInterface(gfx.dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        RELEASE(mt);
    }
    hr = MFCreateDXGIDeviceManager(&token, &mgr);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)gfx.dev, token);
    if (SUCCEEDED(hr))
        hr = MFCreateAttributes(&attr, 3);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_DXGI_MANAGER, (IUnknown *)mgr);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUnknown(attr, &MF_MEDIA_ENGINE_CALLBACK, (IUnknown *)&notify);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUINT32(attr, &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (SUCCEEDED(hr))
        hr = CoCreateInstance(&CLSID_MFMediaEngineClassFactory, NULL, CLSCTX_INPROC_SERVER,
                              &IID_IMFMediaEngineClassFactory, (void **)&cf);
    if (SUCCEEDED(hr))
        hr = IMFMediaEngineClassFactory_CreateInstance(cf, LW_ME_FLAGS, attr, &eng);
    RELEASE(cf);
    RELEASE(attr);
    if (SUCCEEDED(hr)) {
        IMFMediaEngine_SetMuted(eng, TRUE);
        IMFMediaEngine_SetLoop(eng, TRUE);
        url = SysAllocString(path);
        hr = url ? IMFMediaEngine_SetSource(eng, url) : E_OUTOFMEMORY;
        SysFreeString(url);
    }
    if (FAILED(hr)) {
        log_line(L"video: cannot open %s (0x%08X)", path, hr);
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
    if (!eng)
        return;
    if (on) {
        IMFMediaEngine_SetPlaybackRate(eng, source_speed);
        IMFMediaEngine_Play(eng);
    } else {
        IMFMediaEngine_Pause(eng);
    }
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static BOOL v_draw(const FrameInfo *f, BOOL force)
{
    MFVideoNormalizedRect src;
    MFARGB black = {0, 0, 0, 255};
    LONGLONG pts;
    DWORD w = 0, h = 0;
    float o[4], u0, v0, u1, v1;
    RECT dst;
    LARGE_INTEGER now;
    (void)f;

    if (!eng || !gfx.backbuf)
        return FALSE;
    if (IMFMediaEngine_OnVideoStreamTick(eng, &pts) != S_OK && !force)
        return FALSE; /* no new frame */
    /* The frame cap: a 60 fps video (or a 30 fps one at double speed) with
       the cap at 30 shows every other frame. Converting and presenting a
       frame costs about as much as decoding it. The 3/4 leaves room for
       timer jitter, so a video at the cap keeps every frame. */
    QueryPerformanceCounter(&now);
    if (!qpf.QuadPart)
        QueryPerformanceFrequency(&qpf);
    if (!force && source_fps && shown_at && now.QuadPart - shown_at < qpf.QuadPart * 3 / (4 * (LONGLONG)source_fps))
        return FALSE;
    if (FAILED(IMFMediaEngine_GetNativeVideoSize(eng, &w, &h)) || !w || !h)
        return FALSE;
    /* A video smaller than the screen is drawn at its own size and the
       compositor stretches it (gfx_content_size). */
    if (!gfx_content_size(w, h) || !gfx.backbuf)
        return FALSE;
    shown_at = now.QuadPart;

    /* Map the fit mode onto a source crop and a destination rectangle. */
    gfx_fit(w, h, source_fit, o);
    u0 = o[2];
    v0 = o[3];
    u1 = o[2] + o[0];
    v1 = o[3] + o[1];
    src.left = clamp01(u0);
    src.top = clamp01(v0);
    src.right = clamp01(u1);
    src.bottom = clamp01(v1);
    dst.left = (LONG)((src.left - u0) / (u1 - u0) * gfx.bw + 0.5f);
    dst.top = (LONG)((src.top - v0) / (v1 - v0) * gfx.bh + 0.5f);
    dst.right = (LONG)((src.right - u0) / (u1 - u0) * gfx.bw + 0.5f);
    dst.bottom = (LONG)((src.bottom - v0) / (v1 - v0) * gfx.bh + 0.5f);
    if (dst.left > 0 || dst.top > 0 || dst.right < (LONG)gfx.bw || dst.bottom < (LONG)gfx.bh)
        gfx_clear(); /* letterbox bars */
    gfx_unbind();
    return SUCCEEDED(IMFMediaEngine_TransferVideoFrame(eng, (IUnknown *)gfx.backbuf, &src, &dst, &black));
}

static BOOL v_failed(void) { return broken != 0; }

void video_preview_seek(double at)
{
    double len;
    int i;
    for (i = 0; i < 100 && eng && IMFMediaEngine_GetReadyState(eng) < MF_MEDIA_ENGINE_READY_HAVE_METADATA; i++)
        Sleep(30);
    if (!eng)
        return;
    len = IMFMediaEngine_GetDuration(eng);
    if (!(len > 0)) /* unknown (NaN) */
        return;
    if (at > len / 3)
        at = len / 3;
    if (FAILED(IMFMediaEngine_SetCurrentTime(eng, at)))
        return;
    for (i = 0; i < 100 && IMFMediaEngine_IsSeeking(eng); i++)
        Sleep(30);
}

/* Parking closes the whole Media Foundation pipeline (tens of MB and a
   dozen threads) while the video is paused for a long time. The swap chain
   keeps showing the last frame, so nothing visible changes. */
static void v_park(BOOL park)
{
    SourceInfo info;
    if (park) {
        if (!eng)
            return;
        parked_at = IMFMediaEngine_GetCurrentTime(eng);
        v_close();
    } else if (!eng && parked_path[0]) {
        memset(&info, 0, sizeof info);
        if (v_open(parked_path, &info) && parked_at > 0)
            IMFMediaEngine_SetCurrentTime(eng, parked_at);
    }
}

const Source src_video = {v_open, v_close, v_draw, v_play, v_failed, v_park};
