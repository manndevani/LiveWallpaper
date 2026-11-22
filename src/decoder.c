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

static HRESULT make_engine(void)
{
    IMFAttributes *attr = NULL;
    IMFMediaEngineClassFactory *cf = NULL;
    IMFMediaEngineEx *ex = NULL;
    IMFByteStream *bs = NULL;
    WCHAR name[32];
    BSTR url;
    UINT token = 0;
    HRESULT hr = MFCreateDXGIDeviceManager(&token, &mgr);
    if (SUCCEEDED(hr))
        hr = IMFDXGIDeviceManager_ResetDevice(mgr, (IUnknown *)dev, token);
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

    fs.s.lpVtbl = &fs_vtbl;
    fs.file = (HANDLE)(ULONG_PTR)sh->file;
    InitializeCriticalSection(&fs.lock);
    {
        LARGE_INTEGER n;
        if (!GetFileSizeEx(fs.file, &n))
            return HRESULT_FROM_WIN32(GetLastError());
        fs.size = (ULONGLONG)n.QuadPart;
    }
    hr = MFCreateMFByteStreamOnStream(&fs.s, &bs);
    if (SUCCEEDED(hr))
        hr = IMFMediaEngine_QueryInterface(eng, &IID_IMFMediaEngineEx, (void **)&ex);
    /* Only the extension matters: it picks the container format. */
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

static void drop_texture(void)
{
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
    if (SUCCEEDED(hr))
        hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)tex, NULL, &rtv);
    if (FAILED(hr))
        drop_texture();
    return hr;
}

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

/* Draws the current frame into the shared texture, fitted as asked. The
   texture is ours while we hold key 0. Returns DRAWN, NOT_YET (no frame
   decoded yet) or TAKEN (the engine hasn't collected the last frame). */
enum { NOT_YET, DRAWN, TAKEN };
static int draw(void)
{
    static const float black4[4] = {0, 0, 0, 1};
    MFVideoNormalizedRect src;
    MFARGB black = {0, 0, 0, 255};
    DWORD w = 0, h = 0;
    float o[4], u0, v0, u1, v1;
    RECT dst;
    HRESULT hr;

    if (IMFMediaEngine_GetReadyState(eng) < MF_MEDIA_ENGINE_READY_HAVE_CURRENT_DATA ||
        FAILED(IMFMediaEngine_GetNativeVideoSize(eng, &w, &h)) || !w || !h)
        return NOT_YET;
    if (IDXGIKeyedMutex_AcquireSync(km, 0, 0) != S_OK)
        return TAKEN;
    gfx.bw = sh->tex_w;
    gfx.bh = sh->tex_h;
    gfx_fit(w, h, (Fit)sh->fit, o);
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
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black4); /* letterbox bars */
    hr = IMFMediaEngine_TransferVideoFrame(eng, (IUnknown *)tex, &src, &dst, &black);
    IDXGIKeyedMutex_ReleaseSync(km, SUCCEEDED(hr) ? 1 : 0);
    return SUCCEEDED(hr) ? DRAWN : NOT_YET;
}

static void report(void)
{
    DWORD w = 0, h = 0;
    double len = IMFMediaEngine_GetDuration(eng);
    if (SUCCEEDED(IMFMediaEngine_GetNativeVideoSize(eng, &w, &h))) {
        sh->video_w = w;
        sh->video_h = h;
    }
    sh->now = IMFMediaEngine_GetCurrentTime(eng);
    sh->length = len == len && len > 0 ? len : 0; /* NaN while unknown */
}

int decoder_run(const WCHAR *args)
{
    HANDLE share, kick, parent, timer, waits[3];
    LONG tex_gen = 0, seek_gen = 0, redraw_gen = 0, playing = 0;
    float speed = 1.0f;
    BOOL seeking = FALSE, want = FALSE, ticking = FALSE;
    int drawn;
    LARGE_INTEGER qpf, now, due;
    LONGLONG shown_at = 0, play_since = 0;
    BOOL trimmed = FALSE;
    LONGLONG pts;
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
    hr = make_engine();
    if (FAILED(hr))
        return fail(DECODE_AT_SOURCE, hr);
    InterlockedExchange(&sh->state, DECODE_READY);

    QueryPerformanceFrequency(&qpf);
    timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer)
        timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    due.QuadPart = 0;
    waits[0] = kick;
    waits[1] = timer;
    waits[2] = parent;

    for (;;) {
        /* Play while asked to, or while something is still on its way (the
           first frame, a seek, a new texture); otherwise sleep until kicked. */
        BOOL busy = playing || seeking || want || sh->length == 0;
        DWORD r;
        if (busy != ticking) { /* check for new frames at 60 Hz, only while busy */
            ticking = busy;
            if (busy)
                SetWaitableTimer(timer, &due, 1000 / 60, NULL, NULL, FALSE);
            else
                CancelWaitableTimer(timer);
        }
        r = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0 + 2 || sh->quit)
            break; /* the engine is gone or done with us */
        if (InterlockedCompareExchange(&broken, 0, 0)) {
            IMFMediaError *me = NULL;
            hr = E_FAIL;
            if (SUCCEEDED(IMFMediaEngine_GetError(eng, &me)) && me) {
                hr = IMFMediaError_GetExtendedErrorCode(me);
                RELEASE(me);
            }
            fail(DECODE_AT_PLAYBACK, hr);
            break;
        }
        if (sh->tex_gen != tex_gen) {
            tex_gen = sh->tex_gen;
            hr = open_texture();
            if (FAILED(hr)) {
                fail(DECODE_AT_TEXTURE, hr);
                break;
            }
            InterlockedExchange(&sh->tex_seen, tex_gen);
            want = TRUE; /* fill it, also while paused */
        }
        if (sh->redraw_gen != redraw_gen) {
            redraw_gen = sh->redraw_gen;
            want = TRUE;
        }
        if (sh->playing != playing || sh->speed != speed) {
            playing = sh->playing;
            speed = sh->speed;
            if (playing) {
                IMFMediaEngine_SetPlaybackRate(eng, speed);
                IMFMediaEngine_Play(eng);
            } else {
                IMFMediaEngine_Pause(eng);
            }
        }
        if (sh->seek_gen != seek_gen &&
            IMFMediaEngine_GetReadyState(eng) >= MF_MEDIA_ENGINE_READY_HAVE_METADATA) {
            seek_gen = sh->seek_gen;
            IMFMediaEngine_SetCurrentTime(eng, sh->seek_to);
            seeking = TRUE;
        }
        if (seeking && !IMFMediaEngine_IsSeeking(eng)) {
            seeking = FALSE;
            want = TRUE;
            InterlockedExchange(&sh->seek_done, seek_gen);
        }
        report();

        QueryPerformanceCounter(&now);
        /* Hand back memory once, 5 s into playing, as the engine does: most
           of what starting up touched (~30 MB) isn't needed again. */
        if (playing && !play_since)
            play_since = now.QuadPart;
        if (!trimmed && play_since && now.QuadPart - play_since >= 5 * qpf.QuadPart) {
            HeapCompact(GetProcessHeap(), 0);
            SetProcessWorkingSetSize(GetCurrentProcess(), (SIZE_T)-1, (SIZE_T)-1);
            trimmed = TRUE;
        }
        if (!tex || seeking)
            continue;
        /* The frame cap: with a 60 fps video and the cap at 30, every other
           frame. The 3/4 leaves room for timer jitter. */
        if (IMFMediaEngine_OnVideoStreamTick(eng, &pts) == S_OK) {
            if (!sh->fps_cap || !shown_at ||
                now.QuadPart - shown_at >= qpf.QuadPart * 3 / (4 * (LONGLONG)sh->fps_cap))
                want = TRUE;
        }
        if (!want)
            continue;
        drawn = draw();
        if (drawn == DRAWN) {
            want = FALSE;
            shown_at = now.QuadPart;
        } else if (drawn == TAKEN && !playing) {
            /* Paused, and the engine hasn't collected a frame that is
               already the current one: nothing newer is coming. */
            want = FALSE;
        }
    }
    drop_texture();
    if (eng) {
        IMFMediaEngine_Shutdown(eng);
        RELEASE(eng);
    }
    RELEASE(mgr);
    RELEASE(ctx);
    RELEASE(dev);
    return 0;
}
