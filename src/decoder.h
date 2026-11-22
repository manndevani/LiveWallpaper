/* decoder.h - the video helper process ("livewall.exe -decode").
 *
 * Decoding a file is where a booby-trapped video could take over a program,
 * so it happens in a separate process with almost no rights (sandbox.c): an
 * AppContainer with no network and no access to the user's files. It gets
 * exactly one open file handle, and it draws into one texture that both
 * processes share on the graphics card, so frames never leave the card (a 4K
 * video costs the same as in-process).
 *
 * The two sides share one block of memory (DecodeShare) and never send
 * messages: each side writes its own fields and reads the other's. Every
 * new value is followed by SetEvent(kick), so the helper wakes up for it.
 *
 * The texture has a keyed mutex: key 1 means "a new frame is in it" (the
 * helper releases with 1 after drawing), key 0 means "the engine has taken
 * it" (the engine releases with 0 after copying). Neither side ever waits for
 * the other while drawing: both ask with a timeout of 0. */
#pragma once
#include "common.h"

#define DECODE_FLAG L"decode"

typedef struct {
    /* written by the engine (the helper only reads these) */
    LUID adapter;         /* the graphics card to decode on: the engine's */
    UINT64 file;          /* the video, open for reading (a handle in the helper) */
    WCHAR ext[16];        /* its extension, which tells Media Foundation the format */
    volatile LONG tex_gen;/* goes up when tex changes */
    UINT64 tex;           /* the shared texture (NT handle in the helper) */
    UINT tex_w, tex_h;
    LONG fit;             /* Fit */
    LONG playing;
    float speed;
    UINT fps_cap;
    volatile LONG redraw_gen; /* goes up when the fit changed: draw the frame again */
    double seek_to;       /* applied when seek_gen goes up */
    volatile LONG seek_gen;
    volatile LONG quit;

    /* written by the helper */
    volatile LONG state;  /* DECODE_* */
    HRESULT error;        /* why it failed */
    LONG error_at;        /* where: DECODE_AT_* */
    UINT video_w, video_h;
    double now, length;   /* seconds; length 0 while unknown */
    volatile LONG seek_done; /* = seek_gen once that seek has finished */
    volatile LONG tex_seen;  /* = tex_gen once that texture is in use */
    DWORD test[3];           /* -DLW_TEST_SANDBOX=1: what reading a file, listing a folder and connecting gave */
} DecodeShare;

enum { DECODE_STARTING, DECODE_READY, DECODE_FAILED };
enum { DECODE_AT_NONE, DECODE_AT_SHARE, DECODE_AT_DEVICE, DECODE_AT_ENGINE, DECODE_AT_SOURCE,
       DECODE_AT_PLAYBACK, DECODE_AT_TEXTURE };

/* The helper's main: "-decode <share handle> <kick event handle>". */
int decoder_run(const WCHAR *args);
