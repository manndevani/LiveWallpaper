/* decoder.c - the video helper process; see decoder.h.
 * Runs sandboxed: it can't open files or use the network, and it writes no
 * log (it can't reach the log). What goes wrong is reported in DecodeShare,
 * and the engine logs it. */
#include "common.h"
#include "config.h"
#include "gfx.h"
#include "decoder.h"
#include <d3d11_1.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <mferror.h>

static DecodeShare *sh;

/* ---- an IStream over the one file handle we were given ---------------- */

/* Media Foundation reads from several threads; one lock keeps the position
   and the read together. */
typedef struct {
    IStream s;
    HANDLE file;
    ULONGLONG pos, size;
    CRITICAL_SECTION lock;
} FileStream;

static FileStream fs;

static HRESULT STDMETHODCALLTYPE fs_qi(IStream *t, REFIID r, void **o)
{
    if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_ISequentialStream) || IsEqualIID(r, &IID_IStream)) {
        *o = t;
        return S_OK;
    }
    *o = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE fs_ref(IStream *t)
{
    (void)t;
    return 1; /* static object */
}
static HRESULT STDMETHODCALLTYPE fs_read(IStream *t, void *buf, ULONG n, ULONG *got)
{
    OVERLAPPED o;
    DWORD done = 0;
    BOOL ok;
    (void)t;
    EnterCriticalSection(&fs.lock);
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)fs.pos;
    o.OffsetHigh = (DWORD)(fs.pos >> 32);
    ok = fs.pos >= fs.size || ReadFile(fs.file, buf, n, &done, &o) || GetLastError() == ERROR_HANDLE_EOF;
    fs.pos += done;
    LeaveCriticalSection(&fs.lock);
    if (got)
        *got = done;
    return ok ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}
static HRESULT STDMETHODCALLTYPE fs_write(IStream *t, const void *b, ULONG n, ULONG *w)
{
    (void)t, (void)b, (void)n;
    if (w)
        *w = 0;
    return STG_E_ACCESSDENIED;
}
static HRESULT STDMETHODCALLTYPE fs_seek(IStream *t, LARGE_INTEGER move, DWORD from, ULARGE_INTEGER *now)
{
    LONGLONG base, to;
    (void)t;
    EnterCriticalSection(&fs.lock);
    base = from == STREAM_SEEK_SET ? 0 : from == STREAM_SEEK_CUR ? (LONGLONG)fs.pos : (LONGLONG)fs.size;
    to = base + move.QuadPart;
    if (to >= 0)
        fs.pos = (ULONGLONG)to;
    if (now)
        now->QuadPart = fs.pos;
    LeaveCriticalSection(&fs.lock);
    return to >= 0 ? S_OK : STG_E_INVALIDFUNCTION;
}
static HRESULT STDMETHODCALLTYPE fs_setsize(IStream *t, ULARGE_INTEGER n)
{
    (void)t, (void)n;
    return STG_E_ACCESSDENIED;
}
static HRESULT STDMETHODCALLTYPE fs_copyto(IStream *t, IStream *d, ULARGE_INTEGER n, ULARGE_INTEGER *r, ULARGE_INTEGER *w)
{
    (void)t, (void)d, (void)n, (void)r, (void)w;
    return E_NOTIMPL;
}
static HRESULT STDMETHODCALLTYPE fs_commit(IStream *t, DWORD f)
{
    (void)t, (void)f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_revert(IStream *t)
{
    (void)t;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_lock(IStream *t, ULARGE_INTEGER a, ULARGE_INTEGER b, DWORD c)
{
    (void)t, (void)a, (void)b, (void)c;
    return STG_E_INVALIDFUNCTION;
}
static HRESULT STDMETHODCALLTYPE fs_stat(IStream *t, STATSTG *st, DWORD flags)
{
    (void)t, (void)flags;
    memset(st, 0, sizeof *st); /* no name, even if one is asked for */
    st->type = STGTY_STREAM;
    st->cbSize.QuadPart = fs.size;
    st->grfMode = STGM_READ | STGM_SHARE_DENY_WRITE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE fs_clone(IStream *t, IStream **o)
{
    (void)t;
    *o = NULL;
    return E_NOTIMPL;
}

static IStreamVtbl fs_vtbl = {fs_qi, fs_ref, fs_ref, fs_read, fs_write, fs_seek, fs_setsize,
                              fs_copyto, fs_commit, fs_revert, fs_lock, fs_lock, fs_stat, fs_clone};

/* ---- the media engine's one callback ----------------------------------- */

static volatile LONG broken;

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
    return 1;
}
static HRESULT STDMETHODCALLTYPE n_event(IMFMediaEngineNotify *t, DWORD ev, DWORD_PTR p1, DWORD p2)
{
    (void)t, (void)p1, (void)p2;
    if (ev == MF_MEDIA_ENGINE_EVENT_ERROR)
        InterlockedExchange(&broken, 1);
    return S_OK;
}
static IMFMediaEngineNotifyVtbl n_vtbl = {n_qi, n_ref, n_ref, n_event};
static IMFMediaEngineNotify notify = {&n_vtbl};

/* ---- decoding ---------------------------------------------------------- */

static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static IMFDXGIDeviceManager *mgr;
static IMFMediaEngine *eng;
static ID3D11Texture2D *tex;
static ID3D11RenderTargetView *rtv;
static IDXGIKeyedMutex *km;

#if LW_TEST_SANDBOX
/* Tries what taken-over decoder code would try. In the sandbox each one
   should fail with "access denied" (5, and 10013 for the network). */
static void try_escape(void)
{
    static WCHAR path[MAX_PATH];
    typedef int(WINAPI * StartupFn)(WORD, void *);
    typedef UINT_PTR(WINAPI * SocketFn)(int, int, int);
    typedef int(WINAPI * ConnectFn)(UINT_PTR, const void *, int);
    typedef int(WINAPI * ErrFn)(void);
    WIN32_FIND_DATAW fd;
    HANDLE h;
    HMODULE ws;
    GetEnvironmentVariableW(L"APPDATA", path, MAX_PATH);
    str_cat(path, L"\\livewall\\config.ini", MAX_PATH);
    h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    sh->test[0] = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
    GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH);
    str_cat(path, L"\\Documents\\*", MAX_PATH);
    h = FindFirstFileW(path, &fd);
    sh->test[1] = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE)
        FindClose(h);
    sh->test[2] = 1;
    ws = LoadLibraryExW(L"ws2_32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (ws) {
        static BYTE wsa[512];
        StartupFn start = (StartupFn)(void *)GetProcAddress(ws, "WSAStartup");
        SocketFn sock = (SocketFn)(void *)GetProcAddress(ws, "socket");
        ConnectFn conn = (ConnectFn)(void *)GetProcAddress(ws, "connect");
        ErrFn err = (ErrFn)(void *)GetProcAddress(ws, "WSAGetLastError");
        /* 1.1.1.1:443, a public address */
        static const BYTE addr[16] = {2, 0, 0x01, 0xBB, 1, 1, 1, 1};
        if (start && sock && conn && err && start(0x0202, wsa) == 0) {
            UINT_PTR s = sock(2, 1, 6);
            sh->test[2] = s == (UINT_PTR)~0 ? (DWORD)err() : conn(s, addr, 16) == 0 ? 0 : (DWORD)err();
        }
    }
}
#endif

static int fail(LONG at, HRESULT hr)
{
    sh->error = hr;
    sh->error_at = at;
    InterlockedExchange(&sh->state, DECODE_FAILED);
    return 1;
}

static UINT64 parse_u64(const WCHAR **s)
{
    UINT64 v = 0;
    while (**s == ' ')
        (*s)++;
    for (; **s >= '0' && **s <= '9'; (*s)++)
        v = v * 10 + (UINT64)(**s - '0');
    return v;
}

/* The device has to be on the engine's graphics card to share a texture. */
static HRESULT make_device(void)
{
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *ad = NULL;
    ID3D10Multithread *mt = NULL;
    DXGI_ADAPTER_DESC1 d;
    UINT i, flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac);
    for (i = 0; SUCCEEDED(hr) && IDXGIFactory1_EnumAdapters1(fac, i, &ad) == S_OK; i++) {
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)) && d.AdapterLuid.LowPart == sh->adapter.LowPart &&
            d.AdapterLuid.HighPart == sh->adapter.HighPart)
            break;
        RELEASE(ad);
    }
    RELEASE(fac);
    if (!ad)
        return FAILED(hr) ? hr : DXGI_ERROR_NOT_FOUND;
    hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, levels, 3,
                           D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL,
                               flags & ~D3D11_CREATE_DEVICE_VIDEO_SUPPORT, levels, 3, D3D11_SDK_VERSION, &dev,
                               NULL, &ctx);
    RELEASE(ad);
    if (SUCCEEDED(hr) && SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_ID3D10Multithread, (void **)&mt))) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE); /* the media engine uses it from its threads */
        RELEASE(mt);
    }
    return hr;
}

static HRESULT make_manager(void)
{
    UINT token = 0;
    HRESULT hr;
    RELEASE(mgr);
    hr = MFCreateDXGIDeviceManager(&token, &mgr);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev, token);
    return hr;
}

static HRESULT open_file_stream(void)
{
    LARGE_INTEGER n;
    fs.s.lpVtbl = &fs_vtbl;
    fs.file = (HANDLE)(ULONG_PTR)sh->file;
    InitializeCriticalSection(&fs.lock);
    if (!GetFileSizeEx(fs.file, &n))
        return HRESULT_FROM_WIN32(GetLastError());
    fs.size = (ULONGLONG)n.QuadPart;
    return S_OK;
}

static void drop_texture(void);
static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* Where the video goes in the shared texture, fitted as asked: the part of
   the picture to show (normalized, in the picture as it is seen) and the
   rectangle it fills. What the rectangle doesn't cover stays black. */
static void fit_rects(UINT w, UINT h, float s[4], RECT *dst)
{
    float o[4], u0, v0, u1, v1;
    gfx.bw = sh->tex_w;
    gfx.bh = sh->tex_h;
    gfx_fit(w, h, (Fit)sh->fit, o);
    u0 = o[2];
    v0 = o[3];
    u1 = o[2] + o[0];
    v1 = o[3] + o[1];
    s[0] = clamp01(u0);
    s[1] = clamp01(v0);
    s[2] = clamp01(u1);
    s[3] = clamp01(v1);
    dst->left = (LONG)((s[0] - u0) / (u1 - u0) * gfx.bw + 0.5f);
    dst->top = (LONG)((s[1] - v0) / (v1 - v0) * gfx.bh + 0.5f);
    dst->right = (LONG)((s[2] - u0) / (u1 - u0) * gfx.bw + 0.5f);
    dst->bottom = (LONG)((s[3] - v0) / (v1 - v0) * gfx.bh + 0.5f);
}

enum { NOT_YET, DRAWN, TAKEN };
static LARGE_INTEGER qpf;

/* ---- player 1: source reader + the card's video processor -------------
 * Decodes with a Media Foundation source reader and converts each frame
 * (NV12 or P010) straight into the shared texture with the graphics card's
 * video processor, which also scales, turns and fills the bars. We keep the
 * clock ourselves. This needs about half the graphics memory of the media
 * engine (FINDINGS.md, "Where memory still goes"): the media engine keeps
 * its own queue of finished frames, and the source reader in low-latency
 * mode keeps the decoder's surface pool small. */

#define HNS 10000000LL /* 100 ns units per second */

static struct {
    IMFSourceReader *rd;
    ID3D11VideoDevice *vdev;
    ID3D11VideoContext *vctx;
    ID3D11VideoContext1 *vctx1; /* colour spaces by DXGI name (HDR) */
    ID3D11VideoProcessorEnumerator *en;
    ID3D11VideoProcessor *vp;
    ID3D11VideoProcessorOutputView *ov;
    struct {
        ID3D11Texture2D *t;
        UINT slice;
        ID3D11VideoProcessorInputView *v;
    } views[32];  /* decoders hand out slices of one texture array */
    UINT nviews;
    DXGI_FORMAT fmt;
    UINT fw, fh;  /* the decoded frame */
    RECT ap;      /* the part of it that is picture (no padding) */
    UINT dw, dh;  /* the picture as seen: pixel shape and turn applied */
    UINT rot;     /* quarter turns clockwise to show it upright */
    DXGI_COLOR_SPACE_TYPE space;
    BOOL full_range, bt709;
    LONGLONG frame_dur;
    IMFSample *shown, *next;
    LONGLONG shown_ts, next_ts; /* on our clock, which keeps counting across loops */
    LONGLONG shown_raw, next_raw; /* as in the file */
    LONGLONG start, end;          /* first frame of the file, end of the furthest one read */
    LONGLONG off;                 /* our clock minus the file's: the loops so far */
    LONGLONG length;
    BOOL playing;
    float speed;
    LONGLONG clk_pos, clk_qpc;
    HRESULT err;
} rp;

#define VIDEO_STREAM ((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM)

static void sr_drop_views(void)
{
    UINT i;
    for (i = 0; i < rp.nviews; i++) {
        RELEASE(rp.views[i].v);
        RELEASE(rp.views[i].t);
    }
    rp.nviews = 0;
}

static void sr_drop_processor(void)
{
    sr_drop_views();
    RELEASE(rp.ov);
    RELEASE(rp.vp);
    RELEASE(rp.en);
}

static void sr_close(void)
{
    RELEASE(rp.shown);
    RELEASE(rp.next);
    sr_drop_processor();
    RELEASE(rp.vctx1);
    RELEASE(rp.vctx);
    RELEASE(rp.vdev);
    RELEASE(rp.rd);
}

static DXGI_COLOR_SPACE_TYPE input_space(UINT32 matrix, UINT32 range, UINT32 transfer, UINT32 prim)
{
    BOOL full = range == MFNominalRange_0_255;
    BOOL bt2020 = matrix == MFVideoTransferMatrix_BT2020_10 || matrix == MFVideoTransferMatrix_BT2020_12 ||
                  prim == MFVideoPrimaries_BT2020;
    rp.full_range = full;
    /* No matrix given: HD and up is BT.709, smaller is BT.601, as players do. */
    rp.bt709 = matrix == MFVideoTransferMatrix_BT709 || (matrix != MFVideoTransferMatrix_BT601 && rp.fh >= 720);
    if (bt2020) {
        if (transfer == MFVideoTransFunc_2084)
            return DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020;
        if (transfer == MFVideoTransFunc_HLG)
            return full ? DXGI_COLOR_SPACE_YCBCR_FULL_GHLG_TOPLEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_GHLG_TOPLEFT_P2020;
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P2020 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P2020;
    }
    if (rp.bt709)
        return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709;
    return full ? DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601 : DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601;
}

static HRESULT sr_make_processor(void)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd;
    D3D11_VIDEO_COLOR black;
    ID3D11VideoProcessorEnumerator1 *en1 = NULL;
    UINT f = 0;
    HRESULT hr;
    sr_drop_processor();
    memset(&cd, 0, sizeof cd);
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputWidth = rp.fw;
    cd.InputHeight = rp.fh;
    cd.OutputWidth = sh->tex_w ? sh->tex_w : rp.dw;
    cd.OutputHeight = sh->tex_h ? sh->tex_h : rp.dh;
    cd.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(rp.vdev, &cd, &rp.en);
    if (SUCCEEDED(hr))
        hr = ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(rp.en, rp.fmt, &f);
    if (SUCCEEDED(hr) && !(f & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT))
        hr = MF_E_UNSUPPORTED_D3D_TYPE;
    if (SUCCEEDED(hr))
        hr = ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(rp.en, DXGI_FORMAT_B8G8R8A8_UNORM, &f);
    if (SUCCEEDED(hr) && !(f & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT))
        hr = MF_E_UNSUPPORTED_D3D_TYPE;
    /* Can it convert this colour space (HDR, say) to ordinary screen colour?
       If not, the media engine may know how. */
    if (SUCCEEDED(hr) && rp.vctx1 &&
        SUCCEEDED(ID3D11VideoProcessorEnumerator_QueryInterface(rp.en, &IID_ID3D11VideoProcessorEnumerator1,
                                                                (void **)&en1))) {
        BOOL ok = FALSE;
        if (FAILED(ID3D11VideoProcessorEnumerator1_CheckVideoProcessorFormatConversion(
                en1, rp.fmt, rp.space, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &ok)) ||
            !ok)
            hr = MF_E_UNSUPPORTED_D3D_TYPE;
        RELEASE(en1);
    }
    if (SUCCEEDED(hr))
        hr = ID3D11VideoDevice_CreateVideoProcessor(rp.vdev, rp.en, 0, &rp.vp);
    if (FAILED(hr)) {
        sr_drop_processor();
        return hr;
    }
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(rp.vctx, rp.vp, 0, FALSE);
    ID3D11VideoContext_VideoProcessorSetStreamRotation(rp.vctx, rp.vp, 0, rp.rot != 0,
                                                       (D3D11_VIDEO_PROCESSOR_ROTATION)rp.rot);
    memset(&black, 0, sizeof black);
    black.RGBA.A = 1.0f;
    ID3D11VideoContext_VideoProcessorSetOutputBackgroundColor(rp.vctx, rp.vp, FALSE, &black);
    if (rp.vctx1) {
        ID3D11VideoContext1_VideoProcessorSetStreamColorSpace1(rp.vctx1, rp.vp, 0, rp.space);
        ID3D11VideoContext1_VideoProcessorSetOutputColorSpace1(rp.vctx1, rp.vp, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
    } else {
        D3D11_VIDEO_PROCESSOR_COLOR_SPACE in, out;
        memset(&in, 0, sizeof in);
        memset(&out, 0, sizeof out);
        in.YCbCr_Matrix = rp.bt709 ? 1 : 0;
        in.Nominal_Range = rp.full_range ? D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255 : D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
        out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
        ID3D11VideoContext_VideoProcessorSetStreamColorSpace(rp.vctx, rp.vp, 0, &in);
        ID3D11VideoContext_VideoProcessorSetOutputColorSpace(rp.vctx, rp.vp, &out);
    }
    return S_OK;
}

/* Reads what the decoder now gives: size, the part that is picture, pixel
   shape, turn, colour. Called at the start and when the stream changes. */
static HRESULT sr_type(void)
{
    IMFMediaType *t = NULL, *nat = NULL;
    MFVideoArea area;
    GUID sub;
    UINT64 v = 0, pw = 1, ph = 1, w;
    UINT32 u = 0, matrix = 0, range = 0, transfer = 0, prim = 0;
    HRESULT hr = IMFSourceReader_GetCurrentMediaType(rp.rd, VIDEO_STREAM, &t);
    if (FAILED(hr))
        return hr;
    IMFSourceReader_GetNativeMediaType(rp.rd, VIDEO_STREAM, (DWORD)MF_SOURCE_READER_CURRENT_TYPE_INDEX, &nat);
    hr = IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub);
    rp.fmt = IsEqualGUID(&sub, &MFVideoFormat_P010) ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    if (SUCCEEDED(hr))
        hr = IMFMediaType_GetUINT64(t, &MF_MT_FRAME_SIZE, &v);
    rp.fw = (UINT)(v >> 32);
    rp.fh = (UINT)v;
    if (SUCCEEDED(hr) && (!rp.fw || !rp.fh))
        hr = MF_E_INVALIDMEDIATYPE;
    if (SUCCEEDED(hr)) {
        SetRect(&rp.ap, 0, 0, (int)rp.fw, (int)rp.fh);
        if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area, sizeof area, NULL)) ||
            SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_GEOMETRIC_APERTURE, (UINT8 *)&area, sizeof area, NULL))) {
            LONG x = area.OffsetX.value, y = area.OffsetY.value;
            if (x >= 0 && y >= 0 && area.Area.cx > 0 && area.Area.cy > 0 && x + area.Area.cx <= (LONG)rp.fw &&
                y + area.Area.cy <= (LONG)rp.fh)
                SetRect(&rp.ap, x, y, x + area.Area.cx, y + area.Area.cy);
        }
        if (SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_PIXEL_ASPECT_RATIO, &v)) && (v >> 32) && (UINT32)v) {
            pw = v >> 32;
            ph = (UINT32)v;
        }
        rp.rot = 0;
        if ((nat && SUCCEEDED(IMFMediaType_GetUINT32(nat, &MF_MT_VIDEO_ROTATION, &u))) ||
            SUCCEEDED(IMFMediaType_GetUINT32(t, &MF_MT_VIDEO_ROTATION, &u)))
            rp.rot = (u / 90) & 3; /* turned that far counter-clockwise, so turn it back clockwise */
        if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_YUV_MATRIX, &matrix)) && nat)
            IMFMediaType_GetUINT32(nat, &MF_MT_YUV_MATRIX, &matrix);
        if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_VIDEO_NOMINAL_RANGE, &range)) && nat)
            IMFMediaType_GetUINT32(nat, &MF_MT_VIDEO_NOMINAL_RANGE, &range);
        if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_TRANSFER_FUNCTION, &transfer)) && nat)
            IMFMediaType_GetUINT32(nat, &MF_MT_TRANSFER_FUNCTION, &transfer);
        if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_VIDEO_PRIMARIES, &prim)) && nat)
            IMFMediaType_GetUINT32(nat, &MF_MT_VIDEO_PRIMARIES, &prim);
        rp.space = input_space(matrix, range, transfer, prim);
        if (!rp.frame_dur && SUCCEEDED(IMFMediaType_GetUINT64(t, &MF_MT_FRAME_RATE, &v)) && (v >> 32) && (UINT32)v)
            rp.frame_dur = (LONGLONG)(HNS * (UINT32)v / (v >> 32));
        if (!rp.frame_dur)
            rp.frame_dur = HNS / 30;
        w = (UINT64)(rp.ap.right - rp.ap.left) * pw / ph;
        rp.dw = (UINT)(w ? w : 1);
        rp.dh = (UINT)(rp.ap.bottom - rp.ap.top);
        if (rp.rot & 1) {
            UINT x = rp.dw;
            rp.dw = rp.dh;
            rp.dh = x;
        }
        hr = sr_make_processor();
    }
    RELEASE(nat);
    RELEASE(t);
    return hr;
}

/* The next frame. With loop, the end of the file goes back to its start;
   S_FALSE means no frame (the end, without loop). */
static HRESULT sr_rewind(void)
{
    PROPVARIANT p;
    HRESULT hr;
    memset(&p, 0, sizeof p);
    p.vt = VT_I8;
    hr = IMFSourceReader_SetCurrentPosition(rp.rd, &GUID_NULL, &p);
    rp.off += rp.end - rp.start;
    rp.end = 0;
    return hr;
}

static HRESULT sr_read(IMFSample **out, LONGLONG *ts, BOOL loop)
{
    BOOL looped = FALSE;
    int tries;
    *out = NULL;
    for (tries = 0; tries < 64; tries++) {
        DWORD idx = 0, flags = 0;
        LONGLONG t = 0, d = 0;
        IMFSample *s = NULL;
        HRESULT hr = IMFSourceReader_ReadSample(rp.rd, VIDEO_STREAM, 0, &idx, &flags, &t, &s);
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_ERROR))
            hr = E_FAIL;
        if (SUCCEEDED(hr) && (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))
            hr = sr_type(); /* new size, say: a new processor */
        if (FAILED(hr)) {
            RELEASE(s);
            return hr;
        }
        if (s) {
            if (SUCCEEDED(IMFSample_GetSampleDuration(s, &d)) && d > 0)
                rp.frame_dur = d;
            if (t + rp.frame_dur > rp.end)
                rp.end = t + rp.frame_dur;
            *out = s;
            *ts = t;
            return S_OK;
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            if (!loop || looped || rp.end <= rp.start)
                return S_FALSE; /* nothing came after going back: give up on it */
            hr = sr_rewind();
            if (FAILED(hr))
                return hr;
            looped = TRUE;
        }
        /* a gap in the stream: read on */
    }
    return S_FALSE;
}

static HRESULT sr_read_next(void)
{
    LONGLONG raw = 0;
    HRESULT hr;
    RELEASE(rp.next);
    hr = sr_read(&rp.next, &raw, TRUE);
    rp.next_raw = raw;
    rp.next_ts = raw + rp.off;
    return hr;
}

/* The decoder's texture and slice of a frame, if it is on the card. */
static HRESULT frame_texture(IMFSample *s, ID3D11Texture2D **t, UINT *slice)
{
    IMFMediaBuffer *b = NULL;
    IMFDXGIBuffer *db = NULL;
    HRESULT hr = IMFSample_GetBufferByIndex(s, 0, &b);
    *t = NULL;
    if (SUCCEEDED(hr))
        hr = IMFMediaBuffer_QueryInterface(b, &IID_IMFDXGIBuffer, (void **)&db);
    if (SUCCEEDED(hr))
        hr = IMFDXGIBuffer_GetResource(db, &IID_ID3D11Texture2D, (void **)t);
    if (SUCCEEDED(hr))
        hr = IMFDXGIBuffer_GetSubresourceIndex(db, slice);
    if (FAILED(hr))
        RELEASE(*t);
    RELEASE(db);
    RELEASE(b);
    return hr;
}

typedef HRESULT(WINAPI *ReaderFn)(IMFByteStream *, IMFAttributes *, IMFSourceReader **);

static HRESULT sr_open(void)
{
    static const struct { const WCHAR *ext, *mime; } types[] = {
        {L".mp4", L"video/mp4"},        {L".m4v", L"video/mp4"},       {L".mov", L"video/quicktime"},
        {L".mkv", L"video/x-matroska"}, {L".webm", L"video/webm"},     {L".wmv", L"video/x-ms-wmv"},
        {L".avi", L"video/avi"},
    };
    /* Loaded here, not linked: the engine process never needs it. */
    HMODULE lib = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ReaderFn make = lib ? (ReaderFn)(void *)GetProcAddress(lib, "MFCreateSourceReaderFromByteStream") : NULL;
    IMFAttributes *attr = NULL, *ba = NULL;
    IMFByteStream *bs = NULL;
    IMFMediaType *mt = NULL;
    ID3D11Texture2D *t = NULL;
    PROPVARIANT dur;
    WCHAR name[32];
    UINT slice = 0, i;
    LONGLONG raw = 0;
    HRESULT hr = make ? make_manager() : HRESULT_FROM_WIN32(GetLastError());

    memset(&rp, 0, sizeof rp);
    rp.speed = 1.0f;
    if (SUCCEEDED(hr))
        hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11VideoDevice, (void **)&rp.vdev);
    if (SUCCEEDED(hr))
        hr = ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11VideoContext, (void **)&rp.vctx);
    if (SUCCEEDED(hr))
        ID3D11VideoContext_QueryInterface(rp.vctx, &IID_ID3D11VideoContext1, (void **)&rp.vctx1);
    if (SUCCEEDED(hr))
        hr = MFCreateAttributes(&attr, 3);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUnknown(attr, &MF_SOURCE_READER_D3D_MANAGER, (IUnknown *)mgr);
    if (SUCCEEDED(hr))
        hr = IMFAttributes_SetUINT32(attr, &MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    if (SUCCEEDED(hr)) /* a small surface pool: much less graphics memory, see above */
        hr = IMFAttributes_SetUINT32(attr, &MF_LOW_LATENCY, TRUE);

    fs.pos = 0;
    if (SUCCEEDED(hr))
        hr = MFCreateMFByteStreamOnStream(&fs.s, &bs);
    /* Only the extension matters: it picks the container format. */
    if (SUCCEEDED(hr) && SUCCEEDED(IMFByteStream_QueryInterface(bs, &IID_IMFAttributes, (void **)&ba))) {
        str_copy(name, L"video", 32);
        str_cat(name, sh->ext, 32);
        IMFAttributes_SetString(ba, &MF_BYTESTREAM_ORIGIN_NAME, name);
        for (i = 0; i < sizeof types / sizeof types[0]; i++)
            if (str_ieq(sh->ext, types[i].ext))
                IMFAttributes_SetString(ba, &MF_BYTESTREAM_CONTENT_TYPE, types[i].mime);
        RELEASE(ba);
    }
    if (SUCCEEDED(hr))
        hr = make(bs, attr, &rp.rd);
    RELEASE(bs);
    RELEASE(attr);
    if (SUCCEEDED(hr)) {
        IMFSourceReader_SetStreamSelection(rp.rd, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = IMFSourceReader_SetStreamSelection(rp.rd, VIDEO_STREAM, TRUE);
    }
    if (SUCCEEDED(hr))
        hr = MFCreateMediaType(&mt);
    if (SUCCEEDED(hr))
        hr = IMFMediaType_SetGUID(mt, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
    if (SUCCEEDED(hr)) { /* 8-bit video comes as NV12, 10-bit as P010 */
        IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_NV12);
        hr = IMFSourceReader_SetCurrentMediaType(rp.rd, VIDEO_STREAM, NULL, mt);
        if (FAILED(hr)) {
            IMFMediaType_SetGUID(mt, &MF_MT_SUBTYPE, &MFVideoFormat_P010);
            hr = IMFSourceReader_SetCurrentMediaType(rp.rd, VIDEO_STREAM, NULL, mt);
        }
    }
    RELEASE(mt);
    if (SUCCEEDED(hr))
        hr = sr_type();
    memset(&dur, 0, sizeof dur);
    if (SUCCEEDED(hr) && SUCCEEDED(IMFSourceReader_GetPresentationAttribute(
                             rp.rd, (DWORD)MF_SOURCE_READER_MEDIASOURCE, &MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8)
        rp.length = (LONGLONG)dur.uhVal.QuadPart;

    /* The first frame: it must be on the card, or this way is no good
       (a software decoder, say) and the media engine takes over. */
    if (SUCCEEDED(hr))
        hr = sr_read(&rp.shown, &raw, FALSE);
    if (hr == S_FALSE)
        hr = MF_E_END_OF_STREAM;
    if (SUCCEEDED(hr))
        hr = frame_texture(rp.shown, &t, &slice);
    RELEASE(t);
    if (SUCCEEDED(hr)) {
        rp.start = rp.shown_raw = rp.shown_ts = rp.clk_pos = raw;
        hr = sr_read_next();
    }
    if (SUCCEEDED(hr))
        return S_OK;
    sr_close();
    return hr;
}

static LONGLONG sr_pos(LONGLONG q)
{
    if (!rp.playing)
        return rp.clk_pos;
    return rp.clk_pos + (LONGLONG)((double)(q - rp.clk_qpc) * HNS / (double)qpf.QuadPart * rp.speed);
}

static void sr_play(BOOL on, float speed, LONGLONG q)
{
    if (on && !rp.playing)
        rp.clk_pos = rp.shown_ts; /* go on from the frame on screen */
    else
        rp.clk_pos = sr_pos(q);
    rp.clk_qpc = q;
    rp.playing = on;
    rp.speed = speed > 0.01f ? speed : 1.0f;
}

/* Moves on to the frame that is due. TRUE if that is a different one. */
static BOOL sr_tick(LONGLONG q)
{
    LONGLONG pos;
    BOOL changed = FALSE;
    int n;
    if (!rp.playing || rp.err)
        return FALSE;
    if (!rp.next) { /* the file stopped giving frames: try again now and then */
        rp.err = sr_read_next();
        if (rp.err == S_FALSE)
            rp.err = S_OK;
        if (!rp.next)
            return FALSE;
    }
    pos = sr_pos(q);
    /* Behind (a slow card, a long sleep): skip frames, a few per wakeup. */
    for (n = 0; n < 8 && rp.next && rp.next_ts <= pos; n++) {
        RELEASE(rp.shown);
        rp.shown = rp.next;
        rp.shown_ts = rp.next_ts;
        rp.shown_raw = rp.next_raw;
        rp.next = NULL;
        changed = TRUE;
        rp.err = sr_read_next();
        if (rp.err == S_FALSE)
            rp.err = S_OK;
        if (FAILED(rp.err))
            break;
    }
    /* Far behind, or a jump in the file's times: start counting from here. */
    if (rp.next && (pos - rp.next_ts > HNS || rp.next_ts - pos > 2 * HNS)) {
        rp.clk_pos = rp.next_ts < pos ? rp.shown_ts : rp.next_ts;
        rp.clk_qpc = q;
    }
    return changed;
}

/* How long until the next frame is due, in 100 ns. */
static LONGLONG sr_wake(LONGLONG q)
{
    LONGLONG d;
    if (!rp.playing)
        return 0;
    if (!rp.next)
        return HNS;
    d = (LONGLONG)((double)(rp.next_ts - sr_pos(q)) / rp.speed);
    return d < 1 ? 1 : d > HNS ? HNS : d;
}

static HRESULT sr_seek(double to, LONGLONG q)
{
    LONGLONG target = (LONGLONG)(to * HNS), raw = 0;
    IMFSample *s = NULL;
    PROPVARIANT p;
    HRESULT hr;
    int n;
    RELEASE(rp.next);
    memset(&p, 0, sizeof p);
    p.vt = VT_I8;
    p.hVal.QuadPart = target;
    hr = IMFSourceReader_SetCurrentPosition(rp.rd, &GUID_NULL, &p);
    rp.off = 0;
    rp.end = 0;
    /* It lands on the key frame before; decode up to the frame asked for. */
    for (n = 0; SUCCEEDED(hr) && n < 3000; n++) {
        hr = sr_read(&s, &raw, FALSE);
        if (hr != S_OK)
            break;
        RELEASE(rp.shown);
        rp.shown = s;
        rp.shown_raw = rp.shown_ts = raw;
        if (raw + rp.frame_dur > target)
            break;
    }
    if (SUCCEEDED(hr))
        hr = sr_read_next();
    rp.clk_pos = rp.shown_ts;
    rp.clk_qpc = q;
    return FAILED(hr) ? hr : S_OK;
}

static ID3D11VideoProcessorInputView *sr_view(ID3D11Texture2D *t, UINT slice)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC d;
    ID3D11VideoProcessorInputView *v = NULL;
    UINT i;
    for (i = 0; i < rp.nviews; i++)
        if (rp.views[i].t == t && rp.views[i].slice == slice)
            return rp.views[i].v;
    if (rp.nviews == sizeof rp.views / sizeof rp.views[0])
        sr_drop_views(); /* the decoder made new surfaces; start over */
    memset(&d, 0, sizeof d);
    d.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    d.Texture2D.ArraySlice = slice;
    if (FAILED(ID3D11VideoDevice_CreateVideoProcessorInputView(rp.vdev, (ID3D11Resource *)t, rp.en, &d, &v)))
        return NULL;
    ID3D11Texture2D_AddRef(t);
    rp.views[rp.nviews].t = t;
    rp.views[rp.nviews].slice = slice;
    rp.views[rp.nviews].v = v;
    rp.nviews++;
    return v;
}

static int sr_draw(void)
{
    D3D11_VIDEO_PROCESSOR_STREAM st;
    ID3D11Texture2D *t = NULL;
    ID3D11VideoProcessorInputView *iv;
    IMFSample *s = rp.shown;
    UINT slice = 0, il = 0;
    float n[4], m[4], aw, ah;
    RECT src, dst, all;
    HRESULT hr;

    if (!s || !rp.vp || FAILED(frame_texture(s, &t, &slice)))
        return NOT_YET;
    iv = sr_view(t, slice);
    RELEASE(t); /* the view keeps it */
    if (!iv)
        return NOT_YET;
    if (!rp.ov) {
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od;
        memset(&od, 0, sizeof od);
        od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
        if (FAILED(ID3D11VideoDevice_CreateVideoProcessorOutputView(rp.vdev, (ID3D11Resource *)tex, rp.en, &od,
                                                                    &rp.ov)))
            return NOT_YET;
    }
    if (IDXGIKeyedMutex_AcquireSync(km, 0, 0) != S_OK)
        return TAKEN;

    /* The fit is worked out on the picture as seen; the source rectangle is
       in the decoded frame, before the turn. */
    fit_rects(rp.dw, rp.dh, n, &dst);
    switch (rp.rot) {
    case 1: m[0] = n[1], m[1] = 1 - n[2], m[2] = n[3], m[3] = 1 - n[0]; break;
    case 2: m[0] = 1 - n[2], m[1] = 1 - n[3], m[2] = 1 - n[0], m[3] = 1 - n[1]; break;
    case 3: m[0] = 1 - n[3], m[1] = n[0], m[2] = 1 - n[1], m[3] = n[2]; break;
    default: m[0] = n[0], m[1] = n[1], m[2] = n[2], m[3] = n[3]; break;
    }
    aw = (float)(rp.ap.right - rp.ap.left);
    ah = (float)(rp.ap.bottom - rp.ap.top);
    src.left = rp.ap.left + (LONG)(m[0] * aw + 0.5f);
    src.top = rp.ap.top + (LONG)(m[1] * ah + 0.5f);
    src.right = rp.ap.left + (LONG)(m[2] * aw + 0.5f);
    src.bottom = rp.ap.top + (LONG)(m[3] * ah + 0.5f);
    if (src.right <= src.left)
        src.right = src.left + 1;
    if (src.bottom <= src.top)
        src.bottom = src.top + 1;
    SetRect(&all, 0, 0, (int)sh->tex_w, (int)sh->tex_h);
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(rp.vctx, rp.vp, 0, TRUE, &src);
    ID3D11VideoContext_VideoProcessorSetStreamDestRect(rp.vctx, rp.vp, 0, TRUE, &dst);
    ID3D11VideoContext_VideoProcessorSetOutputTargetRect(rp.vctx, rp.vp, TRUE, &all);
    /* Interlaced frames (old camcorder files): show both fields, top or
       bottom first. */
    if (SUCCEEDED(IMFSample_GetUINT32(s, &MFSampleExtension_Interlaced, &il)) && il) {
        UINT32 bff = 0;
        IMFSample_GetUINT32(s, &MFSampleExtension_BottomFieldFirst, &bff);
        ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(
            rp.vctx, rp.vp, 0,
            bff ? D3D11_VIDEO_FRAME_FORMAT_INTERLACED_BOTTOM_FIELD_FIRST : D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST);
    } else {
        ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(rp.vctx, rp.vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    }
    memset(&st, 0, sizeof st);
    st.Enable = TRUE;
    st.pInputSurface = iv;
    hr = ID3D11VideoContext_VideoProcessorBlt(rp.vctx, rp.vp, rp.ov, 0, 1, &st);
    IDXGIKeyedMutex_ReleaseSync(km, SUCCEEDED(hr) ? 1 : 0);
    if (FAILED(hr))
        rp.err = hr;
    return SUCCEEDED(hr) ? DRAWN : NOT_YET;
}

/* ---- player 2: Media Foundation's media engine -------------------------
 * The fallback: it plays whatever Windows can play (software decoders, no
 * video processor), at about twice the graphics memory. */

static HRESULT me_open(void)
{
    IMFAttributes *attr = NULL;
    IMFMediaEngineClassFactory *cf = NULL;
    IMFMediaEngineEx *ex = NULL;
    IMFByteStream *bs = NULL;
    WCHAR name[32];
    BSTR url;
    HRESULT hr = make_manager();
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
    if (SUCCEEDED(hr)) /* real-time mode queues fewer frames (FINDINGS.md) */
        hr = IMFMediaEngineClassFactory_CreateInstance(cf, MF_MEDIA_ENGINE_REAL_TIME_MODE, attr, &eng);
    RELEASE(cf);
    RELEASE(attr);
    if (FAILED(hr))
        return hr;
    IMFMediaEngine_SetMuted(eng, TRUE);
    IMFMediaEngine_SetLoop(eng, TRUE);

    fs.pos = 0;
    hr = MFCreateMFByteStreamOnStream(&fs.s, &bs);
    if (SUCCEEDED(hr))
        hr = IMFMediaEngine_QueryInterface(eng, &IID_IMFMediaEngineEx, (void **)&ex);
    str_copy(name, L"video", 32);
    str_cat(name, sh->ext, 32);
    url = SysAllocString(name);
    if (SUCCEEDED(hr))
        hr = url ? IMFMediaEngineEx_SetSourceFromByteStream(ex, bs, url) : E_OUTOFMEMORY;
    SysFreeString(url);
    RELEASE(ex);
    RELEASE(bs);
    return hr;
}

static int me_draw(void)
{
    static const float black4[4] = {0, 0, 0, 1};
    MFVideoNormalizedRect src;
    MFARGB black = {0, 0, 0, 255};
    DWORD w = 0, h = 0;
    float s[4];
    RECT dst;
    HRESULT hr;

    if (IMFMediaEngine_GetReadyState(eng) < MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA ||
        FAILED(IMFMediaEngine_GetNativeVideoSize(eng, &w, &h)) || !w || !h)
        return NOT_YET;
    if (!rtv && FAILED(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv)))
        return NOT_YET;
    if (IDXGIKeyedMutex_AcquireSync(km, 0, 0) != S_OK)
        return TAKEN;
    fit_rects(w, h, s, &dst);
    src.left = s[0];
    src.top = s[1];
    src.right = s[2];
    src.bottom = s[3];
    if (dst.left > 0 || dst.top > 0 || dst.right < (LONG)gfx.bw || dst.bottom < (LONG)gfx.bh)
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black4); /* letterbox bars */
    hr = IMFMediaEngine_TransferVideoFrame(eng, (IUnknown *)tex, &src, &dst, &black);
    IDXGIKeyedMutex_ReleaseSync(km, SUCCEEDED(hr) ? 1 : 0);
    return SUCCEEDED(hr) ? DRAWN : NOT_YET;
}

/* ---- the two players behind one face ----------------------------------- */

static BOOL reader; /* player 1, else the media engine */

static void p_play(BOOL on, float speed, LONGLONG q)
{
    if (reader) {
        sr_play(on, speed, q);
    } else if (on) {
        IMFMediaEngine_SetPlaybackRate(eng, speed);
        IMFMediaEngine_Play(eng);
    } else {
        IMFMediaEngine_Pause(eng);
    }
}

static BOOL p_can_seek(void)
{
    return reader || IMFMediaEngine_GetReadyState(eng) >= MF_MEDIA_ENGINE_READY_HAVE_METADATA;
}

static HRESULT p_seek(double to, LONGLONG q)
{
    if (reader)
        return sr_seek(to, q);
    return IMFMediaEngine_SetCurrentTime(eng, to);
}

static BOOL p_seeking(void)
{
    return !reader && IMFMediaEngine_IsSeeking(eng);
}

static BOOL p_tick(LONGLONG q)
{
    LONGLONG pts;
    if (reader)
        return sr_tick(q);
    return IMFMediaEngine_OnVideoStreamTick(eng, &pts) == S_OK;
}

static int p_draw(void)
{
    return reader ? sr_draw() : me_draw();
}

static HRESULT p_broken(void)
{
    HRESULT hr = E_FAIL;
    IMFMediaError *me = NULL;
    if (reader)
        return FAILED(rp.err) ? rp.err : S_OK;
    if (!InterlockedCompareExchange(&broken, 0, 0))
        return S_OK;
    if (SUCCEEDED(IMFMediaEngine_GetError(eng, &me)) && me) {
        hr = IMFMediaError_GetExtendedErrorCode(me);
        RELEASE(me);
    }
    return FAILED(hr) ? hr : E_FAIL;
}

static void p_report(void)
{
    DWORD w = 0, h = 0;
    double len;
    if (reader) {
        sh->video_w = rp.dw;
        sh->video_h = rp.dh;
        sh->now = (double)rp.shown_raw / HNS;
        sh->length = (double)rp.length / HNS;
        return;
    }
    len = IMFMediaEngine_GetDuration(eng);
    if (SUCCEEDED(IMFMediaEngine_GetNativeVideoSize(eng, &w, &h))) {
        sh->video_w = w;
        sh->video_h = h;
    }
    sh->now = IMFMediaEngine_GetCurrentTime(eng);
    sh->length = len == len && len > 0 ? len : 0; /* NaN while unknown */
}

/* How long to sleep while busy, in 100 ns: until the next frame is due, or
   1/60 s while the media engine plays (it only says a frame is new when
   asked). 0: until kicked. */
static LONGLONG p_wake(LONGLONG q)
{
    if (reader)
        return sr_wake(q);
    return HNS / 60;
}

static void p_close(void)
{
    if (reader)
        sr_close();
    if (eng) {
        IMFMediaEngine_Shutdown(eng);
        RELEASE(eng);
    }
}

static void drop_texture(void)
{
    RELEASE(rp.ov); /* made on the old texture */
    RELEASE(rtv);
    RELEASE(km);
    RELEASE(tex);
}

static HRESULT open_texture(void)
{
    ID3D11Device1 *d1 = NULL;
    HANDLE h = (HANDLE)(ULONG_PTR)sh->tex;
    HRESULT hr;
    drop_texture();
    hr = ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **)&d1);
    if (SUCCEEDED(hr))
        hr = ID3D11Device1_OpenSharedResource1(d1, h, &IID_ID3D11Texture2D, (void **)&tex);
    RELEASE(d1);
    CloseHandle(h); /* the texture keeps itself alive */
    if (SUCCEEDED(hr))
        hr = ID3D11Texture2D_QueryInterface(tex, &IID_IDXGIKeyedMutex, (void **)&km);
    if (FAILED(hr))
        drop_texture();
    return hr;
}

static void arm(HANDLE timer, LONGLONG wait)
{
    LARGE_INTEGER due;
    if (wait <= 0) {
        CancelWaitableTimer(timer);
        return;
    }
    due.QuadPart = -wait; /* relative */
    SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
}

int decoder_run(const WCHAR *args)
{
    HANDLE share, kick, parent, timer, waits[3];
    LONG tex_gen = 0, seek_gen = 0, redraw_gen = 0, playing = 0;
    float speed = 1.0f;
    BOOL seeking = FALSE, want = FALSE, pending = FALSE;
    int drawn;
    LARGE_INTEGER now;
    LONGLONG shown_at = 0, play_since = 0;
    BOOL trimmed = FALSE;
    HRESULT hr;

    share = (HANDLE)(ULONG_PTR)parse_u64(&args);
    kick = (HANDLE)(ULONG_PTR)parse_u64(&args);
    parent = (HANDLE)(ULONG_PTR)parse_u64(&args);
    sh = (DecodeShare *)MapViewOfFile(share, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(DecodeShare));
    if (!sh)
        return 1;
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
#if LW_TEST_SANDBOX
    try_escape();
#endif

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr))
        hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(hr))
        return fail(DECODE_AT_ENGINE, hr);
    hr = make_device();
    if (FAILED(hr))
        return fail(DECODE_AT_DEVICE, hr);
    hr = open_file_stream();
    if (FAILED(hr))
        return fail(DECODE_AT_SOURCE, hr);
#if LW_FORCE_MEDIA_ENGINE
    hr = E_ABORT;
#else
    hr = sr_open();
#endif
    reader = SUCCEEDED(hr);
    sh->player = reader ? DECODE_PLAYER_READER : DECODE_PLAYER_ENGINE;
    if (!reader) {
        sh->fallback = hr;
        hr = me_open();
        if (FAILED(hr))
            return fail(DECODE_AT_SOURCE, hr);
    }
    p_report();
    InterlockedExchange(&sh->state, DECODE_READY);

    QueryPerformanceFrequency(&qpf);
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    waits[0] = kick;
    waits[1] = timer;
    waits[2] = parent;

    for (;;) {
        /* Sleep until kicked, or until there is something to do: the next
           frame, the frame cap letting a waiting frame through, the engine
           collecting a frame, or (media engine) the file still loading. */
        LONGLONG wait = 0, retry = HNS / 60;
        BOOL now_or_tick;
        DWORD r;
        QueryPerformanceCounter(&now);
        if (playing)
            wait = p_wake(now.QuadPart);
        if (want || seeking || (!reader && sh->length == 0)) {
            if (!wait || wait > retry)
                wait = retry; /* try again soon */
        } else if (pending && sh->fps_cap && shown_at) { /* held back by the frame cap */
            LONGLONG open = shown_at + qpf.QuadPart * 3 / (4 * (LONGLONG)sh->fps_cap) - now.QuadPart;
            LONGLONG w = open <= 0 ? 1 : open * HNS / qpf.QuadPart;
            if (!wait || w < wait)
                wait = w;
        }
        arm(timer, wait);
        r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 2 || sh->quit)
            break; /* the engine is gone or done with us */
        QueryPerformanceCounter(&now);
        hr = p_broken();
        if (FAILED(hr)) {
            fail(DECODE_AT_PLAYBACK, hr);
            break;
        }
        /* Draw now, or at the next new frame? The media engine, while it
           plays, only at the next new frame: for a moment after starting or
           a jump it hands over a blank picture (files with B-frames). */
        now_or_tick = reader || !sh->playing;
        if (sh->tex_gen != tex_gen) {
            tex_gen = sh->tex_gen;
            hr = open_texture();
            if (FAILED(hr)) {
                fail(DECODE_AT_TEXTURE, hr);
                break;
            }
            InterlockedExchange(&sh->tex_seen, tex_gen);
            want = now_or_tick; /* fill it, also while paused */
        }
        if (sh->redraw_gen != redraw_gen) {
            redraw_gen = sh->redraw_gen;
            want = now_or_tick;
        }
        if (sh->playing != playing || sh->speed != speed) {
            playing = sh->playing;
            speed = sh->speed;
            p_play(playing, speed, now.QuadPart);
        }
        if (sh->seek_gen != seek_gen && p_can_seek()) {
            seek_gen = sh->seek_gen;
            hr = p_seek(sh->seek_to, now.QuadPart);
            if (FAILED(hr)) {
                fail(DECODE_AT_PLAYBACK, hr);
                break;
            }
            seeking = TRUE;
        }
        if (seeking && !p_seeking()) {
            seeking = FALSE;
            want = now_or_tick;
            pending = FALSE;
            p_report();
            InterlockedExchange(&sh->seek_done, seek_gen);
        }

        /* Hand back memory once, 5 s into playing, as the engine does: most
           of what starting up touched (~30 MB) isn't needed again. */
        if (playing && !play_since)
            play_since = now.QuadPart;
        if (!trimmed && play_since && now.QuadPart - play_since >= 5 * qpf.QuadPart) {
            HeapCompact(GetProcessHeap(), 0);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            trimmed = TRUE;
        }
        /* Ask for frames even while jumping and before there is a texture:
           with B-frames the media engine only finishes a jump while it is
           asked (it never did in the preview before). */
        if (p_tick(now.QuadPart) && !seeking)
            pending = TRUE;
        if (tex && !seeking) {
            /* The frame cap: with a 60 fps video and the cap at 30, every
               other frame. The 3/4 leaves room for timer jitter. */
            if (pending && (!sh->fps_cap || !shown_at ||
                            now.QuadPart - shown_at >= qpf.QuadPart * 3 / (4 * (LONGLONG)sh->fps_cap)))
                want = TRUE;
            if (want) {
                drawn = p_draw();
                if (drawn == DRAWN) {
                    want = pending = FALSE;
                    shown_at = now.QuadPart;
                } else if (drawn == TAKEN && !playing) {
                    /* Paused, and the engine hasn't collected a frame that
                       is already the current one: nothing newer is coming. */
                    want = pending = FALSE;
                }
            }
        }
        p_report();
        hr = p_broken();
        if (FAILED(hr)) {
            fail(DECODE_AT_PLAYBACK, hr);
            break;
        }
    }
    drop_texture();
    p_close();
    RELEASE(mgr);
    RELEASE(ctx);
    RELEASE(dev);
    return 0;
}
