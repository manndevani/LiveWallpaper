/* sandbox.h - starts a copy of livewall.exe with almost no rights, for work
 * on files that could be booby-trapped (decoding videos: decoder.c). */
#pragma once
#include "common.h"

typedef struct {
    HANDLE process;
    HANDLE job; /* closing it ends the process */
} Sandboxed;

/* Starts "livewall.exe <args>" in an AppContainer with no capabilities (no
   network, none of the user's files), in a job that allows no further
   processes, limits memory and ends it when the job handle is closed (or
   this process ends). It inherits exactly the handles in `inherit`, which
   must be inheritable, at the same values. FALSE: the error is logged. */
BOOL sandbox_start(const WCHAR *args, HANDLE *inherit, UINT n, Sandboxed *out);

/* Ends it now (if it is still running) and closes the handles. */
void sandbox_end(Sandboxed *s);
