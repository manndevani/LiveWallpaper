/* src_shader.c - built-in shaders (compiled at build time) and custom .hlsl
 * files (compiled when chosen, using the system's d3dcompiler_47.dll, which
 * is loaded only for that and unloaded right after). */
#include "common.h"
#include "config.h"
#include "source.h"
#include <d3dcompiler.h>
#include "ps_aurora.h"
#include "ps_dunes.h"
#include "ps_nightsky.h"
#include "ps_ripples.h"
#include "ps_glowgrid.h"

static const struct {
    const WCHAR *id;
    const BYTE *code;
    SIZE_T size;
} kBuiltin[] = {
    {L"builtin:aurora", g_ps_aurora, sizeof g_ps_aurora},
    {L"builtin:dunes", g_ps_dunes, sizeof g_ps_dunes},
    {L"builtin:nightsky", g_ps_nightsky, sizeof g_ps_nightsky},
    {L"builtin:ripples", g_ps_ripples, sizeof g_ps_ripples},
    {L"builtin:glowgrid", g_ps_glowgrid, sizeof g_ps_glowgrid},
};

/* Custom shaders are written Shadertoy-style (see docs/CUSTOM_SHADERS.md).
   We wrap the user's file between this prefix and suffix. */
static const char kPrefix[] =
    "cbuffer Frame : register(b0) {\n"
    "    float2 res; float phase; float time;\n"
    "    float2 mouse; float2 click;\n"
    "    float click_age; float mouse_on; float dim; float _pad;\n"
    "    float4 fit;\n"
    "};\n"
    "#define iResolution float3(res, 1.0)\n"
    "#define iTime time\n"
    "#define iMouse float4(mouse.x, res.y - mouse.y, click.x, res.y - click.y)\n"
    "#define vec2 float2\n#define vec3 float3\n#define vec4 float4\n"
    "#define mat2 float2x2\n#define mat3 float3x3\n"
    "#define fract frac\n#define mix lerp\n"
    "#define mod(x, y) ((x) - (y) * floor((x) / (y)))\n"
    "#line 1\n";
static const char kSuffix[] =
    "\nfloat4 lw_main(float4 pos : SV_Position) : SV_Target {\n"
    "    float4 c = float4(0, 0, 0, 1);\n"
    "    mainImage(c, float2(pos.x, res.y - pos.y));\n"
    "    return float4(saturate(c.rgb) * (1.0 - dim), 1.0);\n"
    "}\n";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR,
                                   UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3D11PixelShader *ps;

#define E_LW_MISSING HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)
#define E_LW_TOO_BIG HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE)

/* Reads a custom shader file, wraps it and compiles it. The blobs belong to
   d3dcompiler_47.dll: the caller releases them, then frees *dll. */
static HRESULT compile_file(const WCHAR *path, ID3DBlob **code, ID3DBlob **err, HMODULE *dll_out)
{
    SIZE_T np = sizeof kPrefix - 1, ns = sizeof kSuffix - 1;
    LARGE_INTEGER size;
    DWORD got = 0;
    HANDLE f;
    char *src;
    HMODULE dll;
    CompileFn compile = NULL;
    HRESULT hr = E_FAIL;

    *dll_out = NULL;
    f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return E_LW_MISSING;
    if (!GetFileSizeEx(f, &size) || size.QuadPart > (1 << 20)) {
        CloseHandle(f);
        return E_LW_TOO_BIG;
    }
    src = (char *)mem_alloc(np + (SIZE_T)size.QuadPart + ns + 1);
    if (!src) {
        CloseHandle(f);
        return E_OUTOFMEMORY;
    }
    memcpy(src, kPrefix, np);
    ReadFile(f, src + np, (DWORD)size.QuadPart, &got, NULL);
    CloseHandle(f);
    if (got >= 3 && (BYTE)src[np] == 0xEF && (BYTE)src[np + 1] == 0xBB && (BYTE)src[np + 2] == 0xBF)
        src[np] = src[np + 1] = src[np + 2] = ' '; /* UTF-8 BOM */
    memcpy(src + np + got, kSuffix, ns);

    dll = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (dll)
        compile = (CompileFn)(void *)GetProcAddress(dll, "D3DCompile");
    if (compile)
        hr = compile(src, np + got + ns, "shader", NULL, NULL, "lw_main", "ps_4_0",
                     D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code, err);
    *dll_out = dll;
    mem_free(src);
    return hr;
}

static BOOL compile_custom(const WCHAR *path)
{
    ID3DBlob *code = NULL, *err = NULL;
    HMODULE dll;
    HRESULT hr = compile_file(path, &code, &err, &dll);
    if (FAILED(hr)) {
        log_line(L"shader %s did not compile:", path);
        if (err)
            log_text((const char *)ID3D10Blob_GetBufferPointer(err), (int)ID3D10Blob_GetBufferSize(err) - 1);
    } else {
        hr = ID3D11Device_CreatePixelShader(gfx.dev, ID3D10Blob_GetBufferPointer(code),
                                            ID3D10Blob_GetBufferSize(code), NULL, &ps);
    }
    RELEASE(code);
    RELEASE(err);
    if (dll)
        FreeLibrary(dll);
    return SUCCEEDED(hr);
}

static BOOL starts(const char *s, const char *with)
{
    for (; *with; s++, with++)
        if (*s != *with)
            return FALSE;
    return TRUE;
}

static const char *find(const char *s, const char *what) /* strstr */
{
    for (; *s; s++)
        if (starts(s, what))
            return s;
    return NULL;
}

/* Why a custom shader can't be shown, in a short sentence, or FALSE if it
   compiles. Compiler messages look like
     shader(12,5-9): error X3004: undeclared identifier 'foo'
   and become "line 12: undeclared identifier 'foo'". */
BOOL shader_problem(const WCHAR *path, WCHAR *out, int max)
{
    static char msg[300];
    ID3DBlob *code = NULL, *err = NULL;
    HMODULE dll;
    HRESULT hr = compile_file(path, &code, &err, &dll);
    const char *e, *p;
    int line = 0, n = 0;

    out[0] = 0;
    if (SUCCEEDED(hr))
        ;
    else if (hr == E_LW_MISSING)
        str_copy(out, L"the file can't be found", max);
    else if (hr == E_LW_TOO_BIG)
        str_copy(out, L"the file is larger than 1 MB", max);
    else if (!err)
        str_copy(out, L"the shader compiler isn't available", max);
    else {
        e = (const char *)ID3D10Blob_GetBufferPointer(err);
        p = find(e, "error");
        if (!p)
            p = e;
        /* Line number: the "(12," just before this message. */
        for (n = (int)(p - e); n > 0 && e[n - 1] != '\n'; n--)
            ;
        for (; e + n < p && e[n] != '('; n++)
            ;
        if (e + n < p)
            for (n++; e[n] >= '0' && e[n] <= '9'; n++)
                line = line * 10 + (e[n] - '0');
        /* Message: after "error X1234: ". */
        if (starts(p, "error X")) {
            const char *c = find(p, ": ");
            if (c)
                p = c + 2;
        }
        for (n = 0; p[n] && p[n] != '\n' && p[n] != '\r' && n < (int)sizeof msg - 1; n++)
            msg[n] = p[n];
        msg[n] = 0;
        if (find(msg, "'mainImage'")) { /* reported in our suffix, past the file's end */
            str_copy(out, L"the file has no mainImage function (see the custom shader guide)", max);
        } else {
            if (line > 0)
                wsprintfW(out, L"line %d: ", line);
            n = str_len(out);
            MultiByteToWideChar(CP_UTF8, 0, msg, -1, out + n, max - n);
            out[max - 1] = 0;
        }
    }
    RELEASE(code);
    RELEASE(err);
    if (dll)
        FreeLibrary(dll);
    return FAILED(hr);
}

static BOOL sh_open(const WCHAR *id, SourceInfo *info)
{
    UINT i;
    info->animated = TRUE;
    for (i = 0; i < sizeof kBuiltin / sizeof kBuiltin[0]; i++) {
        if (str_ieq(id, kBuiltin[i].id)) {
            const Builtin *b = builtin_find(id);
            info->interactive = b && b->interactive;
            return SUCCEEDED(ID3D11Device_CreatePixelShader(gfx.dev, kBuiltin[i].code, kBuiltin[i].size, NULL, &ps));
        }
    }
    info->interactive = TRUE; /* custom shaders may use iMouse */
    return compile_custom(id);
}

static void sh_close(void) { RELEASE(ps); }

static BOOL sh_draw(const FrameInfo *f, BOOL force)
{
    FrameConsts c;
    (void)force;
    if (!ps || !gfx.rtv)
        return FALSE;
    gfx_consts(&c, f);
    gfx_draw(ps, NULL, &c);
    return TRUE;
}

const Source src_shader = {sh_open, sh_close, sh_draw, NULL, NULL};
