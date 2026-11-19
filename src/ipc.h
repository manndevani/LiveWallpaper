/* ipc.h - how the settings window (and -quit) talk to the running engine.
 * The engine owns a hidden window of class ENGINE_CLASS and answers a few
 * registered window messages. No pipes, no polling. */
#pragma once
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    EngineStatus status;
    UINT fps;  /* frames actually presented in the last second */
    UINT w, h; /* render resolution */
} EngineInfo;

HWND engine_window(void);
EngineInfo engine_query(void);
void engine_reload(void);    /* re-read config.ini */
void engine_pause(int mode); /* 0 resume, 1 pause, 2 toggle */
BOOL engine_start(void);
void engine_stop(void);
const WCHAR *status_text(EngineStatus s);
BOOL spawn_self(const WCHAR *args);

#ifdef __cplusplus
}
#endif
