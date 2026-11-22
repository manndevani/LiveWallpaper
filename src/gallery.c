/* gallery.c - see gallery.h. */
#include "common.h"
#include "gallery.h"
#include <winhttp.h>
#include <bcrypt.h>

#define INDEX_MAX (1u << 20)        /* index.json */
#define PREVIEW_MAX (8u << 20)      /* a preview picture */
#define FILE_MAX (60u << 20)        /* a wallpaper (the gallery allows up to 50 MB) */
#define SIGNED_MAX (64u << 10)      /* signed.txt */

/* The gallery maintainer's public signing keys: X and Y of an ECDSA P-256
   key, as hex. `tools/sign.ps1 -NewKey` in the gallery repo makes a key and
   prints this line. The private half never leaves the maintainer's PC, so
   someone who takes over the GitHub account still can't add or change a
   wallpaper that livewall will show. A second line is a spare key. */
#define STR_(x) #x
#define STR(x) STR_(x)
static const char *const gallery_keys[] = {
    /* manndevani's gallery key, made 2026-11-22 */
    "425d0a3029245532a38069e6227212f1d2fbec94e79ba998063ea6a94186ced4339f9f98eeaf86e122943d861c15ecd645c3b902187fdb6f7fe830d8b44437cb",
#ifdef LW_TEST_KEY /* test builds: a throwaway key */
    STR(LW_TEST_KEY),
#endif
    NULL,
};

/* ---- WinHTTP and BCrypt, loaded on first use --------------------------- */

typedef HINTERNET(WINAPI *OpenFn)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET(WINAPI *ConnectFn)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET(WINAPI *RequestFn)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL(WINAPI *SendFn)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL(WINAPI *ReceiveFn)(HINTERNET, LPVOID);
typedef BOOL(WINAPI *QueryFn)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL(WINAPI *ReadFn)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL(WINAPI *CloseFn)(HINTERNET);
typedef BOOL(WINAPI *SetTimeoutsFn)(HINTERNET, int, int, int, int);
typedef NTSTATUS(WINAPI *AlgFn)(BCRYPT_ALG_HANDLE *, LPCWSTR, LPCWSTR, ULONG);
typedef NTSTATUS(WINAPI *CreateHashFn)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *HashDataFn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *FinishFn)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *DestroyFn)(BCRYPT_HASH_HANDLE);
typedef NTSTATUS(WINAPI *ImportFn)(BCRYPT_ALG_HANDLE, BCRYPT_KEY_HANDLE, LPCWSTR, BCRYPT_KEY_HANDLE *, PUCHAR, ULONG,
                                   ULONG);
typedef NTSTATUS(WINAPI *VerifyFn)(BCRYPT_KEY_HANDLE, VOID *, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef NTSTATUS(WINAPI *DestroyKeyFn)(BCRYPT_KEY_HANDLE);

static struct {
    volatile LONG state; /* 0 not loaded, 1 loading, 2 ready, 3 failed */
    OpenFn open;
    ConnectFn connect;
    RequestFn request;
    SendFn send;
    ReceiveFn receive;
    QueryFn query;
    ReadFn read;
    CloseFn close;
    SetTimeoutsFn timeouts;
    CreateHashFn create_hash;
    HashDataFn hash_data;
    FinishFn finish;
    DestroyFn destroy;
    ImportFn import;
    VerifyFn verify;
    DestroyKeyFn destroy_key;
    BCRYPT_ALG_HANDLE sha256, ecdsa;
    HINTERNET session;
} api;

static BOOL load_api(void)
{
    LONG st = InterlockedCompareExchange(&api.state, 1, 0);
    if (st == 0) {
        HMODULE w = LoadLibraryExW(L"winhttp.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        HMODULE b = LoadLibraryExW(L"bcrypt.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        AlgFn alg = NULL;
        BOOL ok = w && b;
        if (ok) {
            api.open = (OpenFn)(void *)GetProcAddress(w, "WinHttpOpen");
            api.connect = (ConnectFn)(void *)GetProcAddress(w, "WinHttpConnect");
            api.request = (RequestFn)(void *)GetProcAddress(w, "WinHttpOpenRequest");
            api.send = (SendFn)(void *)GetProcAddress(w, "WinHttpSendRequest");
            api.receive = (ReceiveFn)(void *)GetProcAddress(w, "WinHttpReceiveResponse");
            api.query = (QueryFn)(void *)GetProcAddress(w, "WinHttpQueryHeaders");
            api.read = (ReadFn)(void *)GetProcAddress(w, "WinHttpReadData");
            api.close = (CloseFn)(void *)GetProcAddress(w, "WinHttpCloseHandle");
            api.timeouts = (SetTimeoutsFn)(void *)GetProcAddress(w, "WinHttpSetTimeouts");
            alg = (AlgFn)(void *)GetProcAddress(b, "BCryptOpenAlgorithmProvider");
            api.create_hash = (CreateHashFn)(void *)GetProcAddress(b, "BCryptCreateHash");
            api.hash_data = (HashDataFn)(void *)GetProcAddress(b, "BCryptHashData");
            api.finish = (FinishFn)(void *)GetProcAddress(b, "BCryptFinishHash");
            api.destroy = (DestroyFn)(void *)GetProcAddress(b, "BCryptDestroyHash");
            api.import = (ImportFn)(void *)GetProcAddress(b, "BCryptImportKeyPair");
            api.verify = (VerifyFn)(void *)GetProcAddress(b, "BCryptVerifySignature");
            api.destroy_key = (DestroyKeyFn)(void *)GetProcAddress(b, "BCryptDestroyKey");
            ok = api.open && api.connect && api.request && api.send && api.receive && api.query && api.read &&
                 api.close && api.timeouts && alg && api.create_hash && api.hash_data && api.finish && api.destroy &&
                 api.import && api.verify && api.destroy_key;
        }
        if (ok)
            ok = alg(&api.sha256, BCRYPT_SHA256_ALGORITHM, NULL, 0) >= 0 &&
                 alg(&api.ecdsa, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0) >= 0;
        if (ok) {
            /* 4 = WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY: the system's proxy settings. */
            api.session = api.open(L"livewall", 4, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            ok = api.session != NULL;
            if (ok)
                api.timeouts(api.session, 10000, 10000, 15000, 30000);
        }
        InterlockedExchange(&api.state, ok ? 2 : 3);
        return ok;
    }
    while (st == 1) { /* another thread is loading it */
        Sleep(10);
        st = InterlockedCompareExchange(&api.state, 0, 0);
    }
    return st == 2;
}

/* ---- SHA-256 ----------------------------------------------------------- */

typedef struct {
    BCRYPT_HASH_HANDLE h;
    BOOL ok, made;
} Hash;

static void hash_begin(Hash *x) { x->ok = x->made = api.create_hash(api.sha256, &x->h, NULL, 0, NULL, 0, 0) >= 0; }

static void hash_add(Hash *x, const BYTE *p, DWORD n)
{
    if (x->ok && n)
        x->ok = api.hash_data(x->h, (PUCHAR)p, n, 0) >= 0;
}

static BOOL hash_finish(Hash *x, BYTE d[32])
{
    BOOL ok = x->ok && api.finish(x->h, d, 32, 0) >= 0;
    if (x->made)
        api.destroy(x->h);
    x->made = FALSE;
    return ok;
}

/* Finishes and compares with the expected lowercase hex. */
static BOOL hash_matches(Hash *x, const char *hex)
{
    static const char digits[] = "0123456789abcdef";
    BYTE d[32];
    int i;
    if (!hash_finish(x, d))
        return FALSE;
    for (i = 0; i < 32; i++)
        if (hex[i * 2] != digits[d[i] >> 4] || hex[i * 2 + 1] != digits[d[i] & 15])
            return FALSE;
    return TRUE;
}

static BOOL file_matches(const WCHAR *path, const char *hex)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    BYTE *buf;
    DWORD got = 0;
    Hash x;
    BOOL ok = TRUE;
    if (f == INVALID_HANDLE_VALUE)
        return FALSE;
    buf = (BYTE *)mem_alloc(64 * 1024);
    hash_begin(&x);
    while (buf) {
        ok = ReadFile(f, buf, 64 * 1024, &got, NULL);
        if (!ok || !got)
            break;
        hash_add(&x, buf, got);
    }
    CloseHandle(f);
    mem_free(buf);
    if (!buf || !ok) {
        if (x.made)
            api.destroy(x.h);
        return FALSE;
    }
    return hash_matches(&x, hex);
}

/* ---- HTTPS ------------------------------------------------------------- */

/* GET https://GALLERY_HOST/GALLERY_ROOT<rel>. On success *out holds the body
   (mem_free it) and *len its size. Fails over max bytes. */
static BOOL http_get(const WCHAR *rel, BYTE **out, DWORD *len, DWORD max)
{
    WCHAR path[300];
    HINTERNET c = NULL, r = NULL;
    DWORD status = 0, size = sizeof status, cap = 64 * 1024, n = 0, got = 0;
    BYTE *buf = NULL;
    BOOL ok = FALSE;

    *out = NULL;
    *len = 0;
    if (!load_api() || str_len(rel) > 200)
        return FALSE;
    str_copy(path, GALLERY_ROOT, 300);
    str_cat(path, rel, 300);
    c = api.connect(api.session, GALLERY_HOST, GALLERY_PORT, 0);
    if (c)
        r = api.request(c, L"GET", path, NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, GALLERY_SECURE);
    if (r && api.send(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        api.receive(r, NULL) &&
        api.query(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                  &size, WINHTTP_NO_HEADER_INDEX) &&
        status == 200) {
        buf = (BYTE *)mem_alloc(cap);
        ok = buf != NULL;
        while (ok) {
            if (n == cap) { /* grow */
                BYTE *bigger;
                if (cap >= max) {
                    ok = FALSE;
                    break;
                }
                cap = cap * 2 > max ? max : cap * 2;
                bigger = (BYTE *)mem_alloc(cap);
                if (!bigger) {
                    ok = FALSE;
                    break;
                }
                memcpy(bigger, buf, n);
                mem_free(buf);
                buf = bigger;
            }
            if (!api.read(r, buf + n, cap - n, &got)) {
                ok = FALSE;
                break;
            }
            if (!got)
                break;
            n += got;
        }
    }
    if (r)
        api.close(r);
    if (c)
        api.close(c);
    if (!ok) {
        mem_free(buf);
        return FALSE;
    }
    *out = buf;
    *len = n;
    return TRUE;
}

/* Downloads rel, checks it against hex, and saves it to path (through a
   .part file, so a broken download never sits under the real name). */
static BOOL download_to(const WCHAR *rel, const char *hex, const WCHAR *path, DWORD max)
{
    WCHAR part[MAX_PATH];
    BYTE *body;
    DWORD len, put = 0;
    HANDLE f;
    Hash x;
    BOOL ok;

    if (!http_get(rel, &body, &len, max))
        return FALSE;
    hash_begin(&x);
    hash_add(&x, body, len);
    ok = hash_matches(&x, hex);
    if (ok) {
        str_copy(part, path, MAX_PATH - 8);
        str_cat(part, L".part", MAX_PATH);
        f = CreateFileW(part, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = f != INVALID_HANDLE_VALUE;
        if (ok) {
            ok = WriteFile(f, body, len, &put, NULL) && put == len;
            CloseHandle(f);
            ok = ok && MoveFileExW(part, path, MOVEFILE_REPLACE_EXISTING);
            if (!ok)
                DeleteFileW(part);
        }
    }
    mem_free(body);
    return ok;
}

/* ---- index.json -------------------------------------------------------- */

typedef struct {
    const char *p, *end;
    int depth;
} Json;

static void ws(Json *j)
{
    while (j->p < j->end && (*j->p == ' ' || *j->p == '\t' || *j->p == '\r' || *j->p == '\n'))
        j->p++;
}

static BOOL eat(Json *j, char c)
{
    ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return TRUE;
    }
    return FALSE;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* A JSON string into out (UTF-16, cut at cap). FALSE if it isn't one. */
static BOOL jstring(Json *j, WCHAR *out, int cap)
{
    char tmp[256]; /* UTF-8, before conversion */
    int n = 0;
    ws(j);
    if (j->p >= j->end || *j->p != '"')
        return FALSE;
    j->p++;
    while (j->p < j->end && *j->p != '"') {
        unsigned c = (BYTE)*j->p++;
        if (c < 0x20)
            return FALSE;
        if (c == '\\') {
            if (j->p >= j->end)
                return FALSE;
            c = (BYTE)*j->p++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                int k, v = 0;
                if (j->end - j->p < 4)
                    return FALSE;
                for (k = 0; k < 4; k++) {
                    int h = hexval(*j->p++);
                    if (h < 0)
                        return FALSE;
                    v = v * 16 + h;
                }
                if (v >= 0xD800 && v <= 0xDFFF)
                    v = '?'; /* pairs aren't needed for names; keep it simple */
                if (v < 0x80) {
                    c = (unsigned)v;
                } else { /* encode as UTF-8 */
                    if (n < (int)sizeof tmp - 3) {
                        if (v < 0x800) {
                            tmp[n++] = (char)(0xC0 | (v >> 6));
                        } else {
                            tmp[n++] = (char)(0xE0 | (v >> 12));
                            tmp[n++] = (char)(0x80 | ((v >> 6) & 63));
                        }
                        tmp[n++] = (char)(0x80 | (v & 63));
                    }
                    continue;
                }
                break;
            }
            default: break; /* \" \\ \/ */
            }
        }
        if (n < (int)sizeof tmp - 1)
            tmp[n++] = (char)c;
    }
    if (j->p >= j->end)
        return FALSE;
    j->p++; /* closing quote */
    n = n ? MultiByteToWideChar(CP_UTF8, 0, tmp, n, out, cap - 1) : 0;
    out[n > 0 ? n : 0] = 0;
    return TRUE;
}

static BOOL jskip(Json *j);

static BOOL jskip_items(Json *j, char close, BOOL keys)
{
    WCHAR k[2];
    if (++j->depth > 16)
        return FALSE;
    if (eat(j, close)) {
        j->depth--;
        return TRUE;
    }
    for (;;) {
        if (keys && (!jstring(j, k, 2) || !eat(j, ':')))
            return FALSE;
        if (!jskip(j))
            return FALSE;
        if (eat(j, close))
            break;
        if (!eat(j, ','))
            return FALSE;
    }
    j->depth--;
    return TRUE;
}

/* Skips any value. */
static BOOL jskip(Json *j)
{
    WCHAR s[2];
    ws(j);
    if (j->p >= j->end)
        return FALSE;
    if (*j->p == '"')
        return jstring(j, s, 2);
    if (eat(j, '{'))
        return jskip_items(j, '}', TRUE);
    if (eat(j, '['))
        return jskip_items(j, ']', FALSE);
    while (j->p < j->end && *j->p != ',' && *j->p != '}' && *j->p != ']' && *j->p != ' ' && *j->p != '\r' &&
           *j->p != '\n')
        j->p++; /* number, true, false, null */
    return TRUE;
}

static BOOL jnumber(Json *j, DWORD *out)
{
    DWORD v = 0;
    ws(j);
    if (j->p >= j->end || *j->p < '0' || *j->p > '9')
        return FALSE;
    while (j->p < j->end && *j->p >= '0' && *j->p <= '9') {
        if (v > 400000000)
            return FALSE;
        v = v * 10 + (DWORD)(*j->p++ - '0');
    }
    *out = v;
    return TRUE;
}

static BOOL id_ok(const WCHAR *s)
{
    int n = str_len(s), i;
    if (n < 3 || n > 40 || s[0] == '-' || s[n - 1] == '-')
        return FALSE;
    for (i = 0; i < n; i++)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-'))
            return FALSE;
    return TRUE;
}

static BOOL hex_ok(const WCHAR *s, char *out)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return FALSE;
        out[i] = (char)s[i];
    }
    out[64] = 0;
    return s[64] == 0;
}

/* One wallpaper object. FALSE only if the JSON itself is broken; *valid says
   whether the entry follows the rules. */
static BOOL jitem(Json *j, GalleryItem *it, BOOL *valid)
{
    WCHAR key[24], file[120], want[120], sha[80], psha[80], preview[120];
    static const WCHAR *const exts[] = {L".hlsl", L".mp4", L".gif", L".png", L".jpg"};
    int i;
    memset(it, 0, sizeof *it);
    file[0] = sha[0] = psha[0] = preview[0] = 0;
    if (!eat(j, '{'))
        return FALSE;
    if (!eat(j, '}')) {
        for (;;) {
            if (!jstring(j, key, 24) || !eat(j, ':'))
                return FALSE;
            if (str_ieq(key, L"id")) { if (!jstring(j, it->id, 44)) return FALSE; }
            else if (str_ieq(key, L"name")) { if (!jstring(j, it->name, 44)) return FALSE; }
            else if (str_ieq(key, L"author")) { if (!jstring(j, it->author, 40)) return FALSE; }
            else if (str_ieq(key, L"file")) { if (!jstring(j, file, 120)) return FALSE; }
            else if (str_ieq(key, L"preview")) { if (!jstring(j, preview, 120)) return FALSE; }
            else if (str_ieq(key, L"sha256")) { if (!jstring(j, sha, 80)) return FALSE; }
            else if (str_ieq(key, L"preview_sha256")) { if (!jstring(j, psha, 80)) return FALSE; }
            else if (str_ieq(key, L"size")) { if (!jnumber(j, &it->size)) return FALSE; }
            else if (str_ieq(key, L"likes")) { if (!jnumber(j, &it->likes)) return FALSE; }
            else if (!jskip(j)) return FALSE;
            if (eat(j, '}'))
                break;
            if (!eat(j, ','))
                return FALSE;
        }
    }
    /* The rules: a proper id, files exactly where the gallery puts them,
       fingerprints present, a sensible size. */
    *valid = FALSE;
    if (!id_ok(it->id) || !it->name[0] || !hex_ok(sha, it->sha) || !hex_ok(psha, it->psha) || !it->size ||
        it->size > FILE_MAX)
        return TRUE;
    for (i = 0; i < 5; i++) {
        wsprintfW(want, L"w/%s/wallpaper%s", it->id, exts[i]);
        if (str_ieq(file, want))
            str_copy(it->ext, exts[i], 8);
    }
    wsprintfW(want, L"w/%s/preview.png", it->id);
    *valid = it->ext[0] && str_ieq(preview, want);
    return TRUE;
}

/* ---- signed.txt -------------------------------------------------------- */

/* The maintainer's signed list of approved wallpapers (tools/sign.ps1 in the
   gallery repo):
       livewall gallery 1\n
       seq <n>\n                          raised at every signing
       <id> <sha256> <preview sha256>\n   one line per wallpaper
       sig <128 hex>\n                    ECDSA P-256 over the SHA-256 of all above
   index.json has the names, likes and so on and isn't signed (the likes
   change every few hours). livewall shows only the wallpapers whose id and
   both fingerprints are in this list. */

#define SIGNED_ITEMS 512

typedef struct {
    char id[44], sha[65], psha[65];
} SignedItem;

static SignedItem signed_items[SIGNED_ITEMS];
static int signed_count;

static BOOL is_lhex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

/* n bytes from 2n hex digits; stops at the first non-digit (or the end). */
static BOOL hex_bytes(const char *s, BYTE *out, int n)
{
    int i, a, b;
    for (i = 0; i < n; i++) {
        if ((a = hexval(s[2 * i])) < 0 || (b = hexval(s[2 * i + 1])) < 0)
            return FALSE;
        out[i] = (BYTE)(a * 16 + b);
    }
    return TRUE;
}

/* TRUE if the SHA-256 of text[0..len) is signed by one of gallery_keys. */
static BOOL signature_ok(const char *text, DWORD len, const char *sighex)
{
    BYTE digest[32], sig[64];
    Hash x;
    int k;
    if (!hex_bytes(sighex, sig, 64))
        return FALSE;
    hash_begin(&x);
    hash_add(&x, (const BYTE *)text, len);
    if (!hash_finish(&x, digest))
        return FALSE;
    for (k = 0; gallery_keys[k]; k++) {
        struct {
            BCRYPT_ECCKEY_BLOB h;
            BYTE xy[64];
        } blob;
        BCRYPT_KEY_HANDLE key = NULL;
        BOOL good;
        int n = 0;
        while (n < 129 && gallery_keys[k][n])
            n++;
        if (n != 128 || !hex_bytes(gallery_keys[k], blob.xy, 64))
            continue;
        blob.h.dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
        blob.h.cbKey = 32;
        if (api.import(api.ecdsa, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, (PUCHAR)&blob, sizeof blob, 0) < 0)
            continue;
        good = api.verify(key, NULL, digest, 32, sig, 64, 0) >= 0;
        api.destroy_key(key);
        if (good)
            return TRUE;
    }
    return FALSE;
}

/* Reads signed.txt. With verify, also checks the signature. Fills items
   (if given, up to SIGNED_ITEMS) and *seq. FALSE if anything is off. */
static BOOL parse_signed(const char *t, DWORD len, BOOL verify, DWORD *seq, SignedItem *items, int *count)
{
    static const char head[] = "livewall gallery 1\nseq ";
    const char *p, *end, *sig;
    DWORD v = 0;
    int n = 0, k;

    if (len < (sizeof head - 1) + 2 + 5 + 128 + 1 || memcmp(t, head, sizeof head - 1) || t[len - 1] != '\n')
        return FALSE;
    sig = t + len - 1 - 128; /* the last line: "sig <hex>\n" */
    if (memcmp(sig - 5, "\nsig ", 5))
        return FALSE;
    for (k = 0; k < 128; k++)
        if (!is_lhex(sig[k]))
            return FALSE;
    end = sig - 4; /* the signed part ends with the '\n' before "sig" */
    if (verify && !signature_ok(t, (DWORD)(end - t), sig))
        return FALSE;

    p = t + sizeof head - 1;
    if (p >= end || *p < '0' || *p > '9')
        return FALSE;
    while (p < end && *p >= '0' && *p <= '9') {
        if (v > 400000000)
            return FALSE;
        v = v * 10 + (DWORD)(*p++ - '0');
    }
    if (p >= end || *p++ != '\n')
        return FALSE;
    while (p < end) {
        SignedItem it;
        for (k = 0; p < end && *p != ' ' && k < 43; k++, p++) {
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-'))
                return FALSE;
            it.id[k] = *p;
        }
        it.id[k] = 0;
        if (k < 3 || k > 40 || end - p < 1 + 64 + 1 + 64 + 1 || *p++ != ' ')
            return FALSE;
        for (k = 0; k < 64; k++, p++) {
            if (!is_lhex(*p))
                return FALSE;
            it.sha[k] = *p;
        }
        it.sha[64] = 0;
        if (*p++ != ' ')
            return FALSE;
        for (k = 0; k < 64; k++, p++) {
            if (!is_lhex(*p))
                return FALSE;
            it.psha[k] = *p;
        }
        it.psha[64] = 0;
        if (*p++ != '\n')
            return FALSE;
        if (items && n < SIGNED_ITEMS)
            items[n++] = it;
    }
    *seq = v;
    if (count)
        *count = n;
    return TRUE;
}

/* Downloads and checks signed.txt. A list older than the last one this PC
   accepted (a lower seq) is refused, so an old list that still had a
   wallpaper the maintainer has since taken off can't be served again. */
static BOOL signed_fetch(void)
{
    WCHAR path[MAX_PATH];
    BYTE *body = NULL, *old;
    DWORD len = 0, seq = 0, oldseq = 0, got = 0;
    HANDLE f;
    BOOL ok;

    signed_count = 0;
    ok = http_get(L"signed.txt", &body, &len, SIGNED_MAX) &&
         parse_signed((const char *)body, len, TRUE, &seq, signed_items, &signed_count);
    if (ok) {
        app_dir(path);
        str_cat(path, L"\\gallery", MAX_PATH);
        CreateDirectoryW(path, NULL);
        str_cat(path, L"\\signed.txt", MAX_PATH);
        f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            old = (BYTE *)mem_alloc(SIGNED_MAX);
            if (old && ReadFile(f, old, SIGNED_MAX, &got, NULL) &&
                parse_signed((const char *)old, got, FALSE, &oldseq, NULL, NULL) && seq < oldseq)
                ok = FALSE;
            mem_free(old);
            CloseHandle(f);
        }
    }
    if (ok) { /* remember it */
        f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            WriteFile(f, body, len, &got, NULL);
            CloseHandle(f);
        }
    }
    if (!ok)
        signed_count = 0;
    mem_free(body);
    return ok;
}

static BOOL is_signed(const GalleryItem *it)
{
    int i, k;
    for (i = 0; i < signed_count; i++) {
        const SignedItem *s = &signed_items[i];
        for (k = 0; s->id[k] && it->id[k] == (WCHAR)(BYTE)s->id[k]; k++)
            ;
        if (!s->id[k] && !it->id[k] && !memcmp(s->sha, it->sha, 64) && !memcmp(s->psha, it->psha, 64))
            return TRUE;
    }
    return FALSE;
}

/* ---- the list ---------------------------------------------------------- */

int gallery_fetch(GalleryItem *items, int max)
{
    BYTE *body;
    DWORD len;
    Json j;
    WCHAR key[24];
    int n = 0;
    BOOL ok = FALSE;

    if (!http_get(L"index.json", &body, &len, INDEX_MAX))
        return -1;
    if (!signed_fetch()) {
        mem_free(body);
        return -2;
    }
    j.p = (const char *)body;
    j.end = j.p + len;
    j.depth = 0;
    if (eat(&j, '{')) {
        for (;;) {
            if (!jstring(&j, key, 24) || !eat(&j, ':'))
                break;
            if (str_ieq(key, L"wallpapers")) {
                if (!eat(&j, '['))
                    break;
                if (!eat(&j, ']')) {
                    for (;;) {
                        BOOL valid = FALSE;
                        if (n >= max) { /* more than we show: skip the rest */
                            GalleryItem spare;
                            if (!jitem(&j, &spare, &valid))
                                break;
                        } else if (!jitem(&j, &items[n], &valid)) {
                            break;
                        } else if (valid && is_signed(&items[n])) {
                            n++;
                        }
                        if (eat(&j, ']'))
                            break;
                        if (!eat(&j, ','))
                            break;
                    }
                }
                ok = TRUE; /* the list was read */
            } else if (!jskip(&j)) {
                break;
            }
            if (eat(&j, '}') || !eat(&j, ','))
                break;
        }
    }
    mem_free(body);
    if (ok) { /* most liked first; equal ones keep the list's order */
        static GalleryItem t;
        int a, b;
        for (a = 1; a < n; a++) {
            t = items[a];
            for (b = a; b > 0 && items[b - 1].likes < t.likes; b--)
                items[b] = items[b - 1];
            items[b] = t;
        }
    }
    return ok ? n : -1;
}

/* ---- files ------------------------------------------------------------- */

/* %APPDATA%\livewall\gallery\<sub>, created when make is set. */
static void gallery_dir(WCHAR *out, const WCHAR *sub, BOOL make)
{
    app_dir(out);
    str_cat(out, L"\\gallery", MAX_PATH);
    if (make)
        CreateDirectoryW(out, NULL);
    str_cat(out, L"\\", MAX_PATH);
    str_cat(out, sub, MAX_PATH);
    if (make)
        CreateDirectoryW(out, NULL);
}

void gallery_preview_path(const GalleryItem *it, WCHAR *out)
{
    gallery_dir(out, L"previews", FALSE);
    str_cat(out, L"\\", MAX_PATH);
    str_cat(out, it->id, MAX_PATH);
    str_cat(out, L".png", MAX_PATH);
}

BOOL gallery_preview(const GalleryItem *it)
{
    WCHAR path[MAX_PATH], rel[80];
    if (!load_api())
        return FALSE;
    gallery_dir(path, L"previews", TRUE);
    gallery_preview_path(it, path);
    if (file_matches(path, it->psha))
        return TRUE;
    wsprintfW(rel, L"w/%s/preview.png", it->id);
    return download_to(rel, it->psha, path, PREVIEW_MAX);
}

void gallery_local_path(const GalleryItem *it, WCHAR *out)
{
    WCHAR name[44];
    int i, n = 0;
    /* The name shown in the library comes from the file name, so keep the
       wallpaper's name but only plain, safe characters. */
    for (i = 0; it->name[i] && n < 40; i++) {
        WCHAR c = it->name[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '\'' ||
            (c == ' ' && n && name[n - 1] != ' '))
            name[n++] = c;
    }
    while (n && name[n - 1] == ' ')
        n--;
    name[n] = 0;
    gallery_dir(out, it->id, FALSE);
    str_cat(out, L"\\", MAX_PATH);
    str_cat(out, n ? name : it->id, MAX_PATH);
    str_cat(out, it->ext, MAX_PATH);
}

BOOL gallery_install(const GalleryItem *it)
{
    WCHAR path[MAX_PATH], rel[80];
    if (!load_api())
        return FALSE;
    gallery_dir(path, it->id, TRUE);
    gallery_local_path(it, path);
    if (file_matches(path, it->sha))
        return TRUE;
    wsprintfW(rel, L"w/%s/wallpaper%s", it->id, it->ext);
    return download_to(rel, it->sha, path, FILE_MAX);
}
