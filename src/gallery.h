/* gallery.h - the community gallery (https://github.com/manndevani/livewall-gallery).
 *
 * Used only by the settings window, from worker threads: every call blocks
 * on the network. Nothing here logs (log_line isn't thread-safe); callers
 * log the results.
 *
 * Safety: downloads only over HTTPS from GALLERY_HOST, with size limits.
 * Only wallpapers in the maintainer's signed list are shown, and every file
 * is checked against the SHA-256 in that list before it is used, and saved
 * under a name livewall makes itself, never one from the download. WinHTTP and BCrypt are loaded on first use, so the wallpaper
 * engine process never loads them. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Release builds: HTTPS to GitHub Pages only. The two test builds never ship. */
#if defined(LW_TEST_NO_GALLERY) /* a host that never answers, as when offline */
#define GALLERY_HOST L"livewall-gallery.invalid"
#define GALLERY_ROOT L"/livewall-gallery/"
#elif defined(LW_TEST_LOCAL_GALLERY) /* a test copy of the site on this PC, plain HTTP */
#define GALLERY_HOST L"127.0.0.1"
#define GALLERY_ROOT L"/"
#define GALLERY_PORT 8765
#define GALLERY_SECURE 0
#else
#define GALLERY_HOST L"manndevani.github.io"
#define GALLERY_ROOT L"/livewall-gallery/"
#endif
#ifndef GALLERY_PORT
#define GALLERY_PORT INTERNET_DEFAULT_HTTPS_PORT
#define GALLERY_SECURE WINHTTP_FLAG_SECURE
#endif
#define GALLERY_SHARE_URL L"https://github.com/manndevani/livewall-gallery/blob/main/SHARING.md"
#define GALLERY_MAX 64

typedef struct {
    WCHAR id[44];     /* folder name: a-z, 0-9 and dashes */
    WCHAR name[44];
    WCHAR author[40];
    WCHAR ext[8];     /* ".hlsl", ".mp4", ".gif", ".png", ".jpg" */
    char sha[65];     /* of the wallpaper file, lowercase hex */
    char psha[65];    /* of the preview picture */
    DWORD size;
    DWORD likes;      /* upvotes on its post in the gallery's Discussions */
} GalleryItem;

/* Downloads and reads index.json and the maintainer's signed list
   (signed.txt). Returns the number of wallpapers; -1 if the gallery can't be
   reached or its list is broken; -2 if the signed list is missing, its
   signature is wrong, or it is older than one seen before. Entries that
   break a rule, or aren't in the signed list with the same fingerprints,
   are left out. */
int gallery_fetch(GalleryItem *items, int max);

/* The preview picture, downloaded to gallery_preview_path if it isn't there
   with the right fingerprint. */
BOOL gallery_preview(const GalleryItem *it);
void gallery_preview_path(const GalleryItem *it, WCHAR *out); /* MAX_PATH */

/* Where the wallpaper lives once installed:
   %APPDATA%\livewall\gallery\<id>\<name><ext>. Touches nothing on disk. */
void gallery_local_path(const GalleryItem *it, WCHAR *out);

/* Downloads the wallpaper, checks its fingerprint, and saves it to
   gallery_local_path. TRUE if it is there with the right fingerprint
   (also when it already was). */
BOOL gallery_install(const GalleryItem *it);

#ifdef __cplusplus
}
#endif
