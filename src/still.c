/* still.c - see still.h.
 *
 * A still picture needs no animation, so instead of keeping a Direct3D
 * device, a window and a texture alive for it, we give it to Windows
 * (IDesktopWallpaper). Windows draws it itself; the engine then only keeps
 * its tray icon. If brightness is below 100%, a dimmed copy is written to
 * %APPDATA%\livewall and that copy is used instead. */
#include "common.h"
#include "config.h"
#include "preview.h"
#include "still.h"
#include <shobjidl.h>
#include <wincodec.h>

/* Writes a dimmed copy of src next to the config. Returns FALSE on failure. */
static BOOL make_dimmed(const WCHAR *src, UINT brightness, BOOL in_temp, WCHAR *out)
{
    IWICImagingFactory *wic = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    BYTE *px = NULL;
    UINT w = 0, h = 0, i, mul = brightness * 256 / 100;
    SIZE_T size;
    WCHAR dir[MAX_PATH], name[48];
    WIN32_FIND_DATAW fd;
    HANDLE find;
    BOOL ok = FALSE;
    HRESULT hr;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                          (void **)&wic);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromFilename(wic, src, NULL, GENERIC_READ,
                                                                         WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(wic, &conv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                                           &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone,
                                                           NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_GetSize(conv, &w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0 || w > 16384 || h > 16384))
        hr = E_FAIL;
    size = (SIZE_T)w * h * 4;
    if (SUCCEEDED(hr)) {
        px = (BYTE *)mem_alloc(size);
        if (!px)
            hr = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_CopyPixels(conv, NULL, w * 4, (UINT)size, px);
    if (SUCCEEDED(hr)) {
        for (i = 0; i < size; i += 4) {
            px[i] = (BYTE)(px[i] * mul >> 8);
            px[i + 1] = (BYTE)(px[i + 1] * mul >> 8);
            px[i + 2] = (BYTE)(px[i + 2] * mul >> 8);
            px[i + 3] = 255;
        }
        /* A new name each time: Windows may cache a file it has seen before.
           Remove the copies from earlier runs first. */
        if (in_temp) { /* see still_apply */
            GetTempPathW(MAX_PATH, dir);
            dir[str_len(dir) - 1] = 0; /* drop the trailing backslash */
        } else {
            app_dir(dir);
        }
        str_copy(out, dir, MAX_PATH);
        str_cat(out, L"\\livewall_still_*.png", MAX_PATH);
        find = FindFirstFileW(out, &fd);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                str_copy(out, dir, MAX_PATH);
                str_cat(out, L"\\", MAX_PATH);
                str_cat(out, fd.cFileName, MAX_PATH);
                DeleteFileW(out);
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }
        wsprintfW(name, L"\\livewall_still_%u.png", GetTickCount());
        str_copy(out, dir, MAX_PATH);
        str_cat(out, name, MAX_PATH);
        ok = preview_save_png(out, w, h, px);
    }
    mem_free(px);
    RELEASE(conv);
    RELEASE(frame);
    RELEASE(dec);
    RELEASE(wic);
    return ok;
}

BOOL still_apply(const Config *c)
{
    static WCHAR path[MAX_PATH];
    IDesktopWallpaper *dw = NULL;
    DESKTOP_WALLPAPER_POSITION pos = c->fit == FIT_CONTAIN ? DWPOS_FIT
                                     : c->fit == FIT_STRETCH ? DWPOS_STRETCH
                                                             : DWPOS_FILL;
    HRESULT hr;

    if (GetFileAttributesW(c->current) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    BOOL dimmed = FALSE;

    if (GetFileAttributesW(c->current) == INVALID_FILE_ATTRIBUTES)
        return FALSE;
    str_copy(path, c->current, MAX_PATH);
    if (c->brightness < 100) {
        dimmed = make_dimmed(c->current, c->brightness, FALSE, path);
        if (!dimmed) {
            log_line(L"could not dim %s; using it as is", c->current);
            str_copy(path, c->current, MAX_PATH);
        }
    }
    hr = CoCreateInstance(&CLSID_DesktopWallpaper, NULL, CLSCTX_LOCAL_SERVER, &IID_IDesktopWallpaper,
                          (void **)&dw);
    if (SUCCEEDED(hr)) {
        IDesktopWallpaper_SetPosition(dw, pos);
        hr = IDesktopWallpaper_SetWallpaper(dw, NULL, path); /* NULL: every monitor */
        /* When livewall runs inside a packaged app, Windows redirects our
           %APPDATA% to a private folder Explorer cannot see (FINDINGS.md).
           The temp folder is not redirected, and Windows keeps its own copy
           of the wallpaper, so a temp file is fine. */
        if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && dimmed &&
            make_dimmed(c->current, c->brightness, TRUE, path))
            hr = IDesktopWallpaper_SetWallpaper(dw, NULL, path);
        IDesktopWallpaper_Release(dw);
    }
    if (FAILED(hr))
        log_line(L"Windows did not accept %s as the wallpaper (0x%08X)", path, (UINT)hr);
    else
        log_line(L"handed %s to Windows as the wallpaper", path);
    return SUCCEEDED(hr);
}
