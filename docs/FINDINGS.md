# Findings

Facts learned while building livewall, with dates. Add to this file; don't rewrite history.

## Getting behind the desktop icons (2026-11)

- Sending `0x052C` to `Progman` (wParam `0xD`, lParam `1`, then `0, 0`) makes Explorer split the wallpaper layer from the icon layer. The message is undocumented but has been stable since Windows 8.
- **Windows 11 24H2+ (verified on 25H2, build 26200): the layout changed.** `SHELLDLL_DefView` (the icons) and a `WorkerW` are now **children of `Progman`**; nothing is a top-level `WorkerW` any more.
  - What works: create our window as a child of `Progman`, then `SetWindowPos(wall, icons, ...)` to put it directly under `SHELLDLL_DefView`.
  - Explorer sometimes reorders its children, so the 500 ms poll re-asserts that z-order.
- **Older builds:** after the message, `SHELLDLL_DefView` lives in a top-level `WorkerW`. The *next* top-level `WorkerW` is the empty wallpaper layer, and we parent to it.
- Explorer restarting destroys our window (it was a child of Progman). The poll notices and attaches again.

## Reentrancy bug (2026-11)

- `SendMessageTimeout` (used for `0x052C`) dispatches *incoming sent messages* while it waits. A timer-driven `poll()` could re-enter itself through that path and create a **second wallpaper window**.
- Fix: a static `busy` guard in `poll()`. Any code path that calls `SendMessageTimeout` must assume it can be re-entered.

## Hybrid GPUs (Optimus) (2026-11)

- On the user's laptop the **NVIDIA RTX A3000 drives the internal display**; the Intel UHD is also present. Creating the device with a null adapter loaded **both** drivers' user-mode DLLs.
- Fix:
  - Pick the adapter whose output is the primary monitor, or `IDXGIFactory6::EnumAdapterByGpuPreference(MINIMUM_POWER)` when "Use integrated GPU" is on.
  - Add `D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS`, which also cut threads.
- Get the factory from the device's adapter, otherwise `CreateSwapChain` can fail across adapters.
- `D3D11_CREATE_DEVICE_SINGLETHREADED` breaks Media Foundation, which needs multithreaded access, so it is not used. `VIDEO_SUPPORT` is requested, with a fallback to creating the device without it.

## Measurements (2026-11-20, Windows 11 25H2, RTX A3000, 1920×1080, 30 fps cap)

Measured with `tools/measure.ps1 -Render`, forced to render even though windows covered the desktop:

| Wallpaper | Private WS | CPU (all cores) | GPU time |
|---|---|---|---|
| Built-in shaders (half res) | 14.3–14.4 MB | 0.08–0.14 % | 26–34 ms/s |
| Custom shader `examples/plasma.hlsl` | 14.4 MB | 0.11 % | 25 ms/s |
| Still image (PNG), before handing it to Windows | 18.1 MB | 0.01 % | 0 (drawn once) |
| Animated GIF 320×180 | 15.8 MB | 0.03 % | 42 ms/s |
| H.264 MP4 640×360 30 fps | **58.9 MB** | 0.21 % | **150 ms/s** |
| Any wallpaper, desktop covered (paused), before trimming | 14.2 MB | ~0.01 % | 0 |

### After the idle-memory work (2026-11-20, same machine)

| State | Private WS | Threads | Notes |
|---|---|---|---|
| Shader, paused (covered) | **0.3 MB** | 16 | working set handed back 2 s after pausing |
| Video, paused | **1.2 MB** | 20 | Media Foundation pipeline closed after 10 s paused; resumes at the same position (verified 29–30 fps right after resume) |
| Still picture (also dimmed) | **0.5 MB** | 9 | handed to Windows; no D3D device, no window |
| Shader, rendering | 14.3 MB | 16 | unchanged; the GPU driver is the floor (~10 MB) |
| Video, rendering | 59–68 MB | 26 | unchanged |

- "Private WS" is what Task Manager shows as Memory. Trimming (`SetProcessWorkingSetSize(-1, -1)` plus `HeapCompact`) moves pages to the standby list. They come back as soft faults when drawing resumes, which is cheap. Committed memory does not shrink. Freeing the decoder (video) and handing pictures to Windows does shrink it.
- Below ~10 MB while *rendering* is not reachable on Windows: the user-mode GPU driver alone is about that size.

### Shader resolution: half vs full (2026-11-20, same machine, 1920×1080)

Half resolution (960×540, stretched by DWM) looked visibly soft on a 1080p screen, so the default became Auto (full resolution up to 2560×1600).

| Shader | Half: GPU | Full: GPU | Memory (both) |
|---|---|---|---|
| Aurora | 27 ms/s | 98 ms/s | 14.4 MB |
| Dunes | 28 ms/s | 104 ms/s | 14.4 MB |

- Full resolution costs about 3.7× the GPU time: roughly 4× the pixels, minus fixed per-frame overhead.
- That is about 3.3 ms of GPU work per frame at 30 fps. Memory and CPU don't change.
- It still pauses at zero cost when covered or on battery.
- Measure with `tools\measure.ps1 -Render -Resolution 1|2`.

## Packaged-app AppData redirection (2026-11-20)

- When livewall is launched from inside a packaged (MSIX) app, such as a terminal hosted by a Store-packaged app, Windows redirects its `%APPDATA%` writes to `%LOCALAPPDATA%\Packages\<app>\LocalCache\Roaming\`. That folder is invisible to Explorer and to the same exe launched normally.
- Result: `IDesktopWallpaper::SetWallpaper` on a file in our AppData fails with `0x80070002` (file not found). `still.c` retries with a copy in `%TEMP%`, which is not redirected.
- It also means the config used during development sessions is not the one the sign-in task will read. The first real sign-in may behave like a first run.

- "GPU time" is the sum of `\GPU Engine(pid_*)\Running Time` across all engines. For video that includes the decode and video-processor engines.
- Shader cost is almost the same whichever shader is used. Present and composition dominate, not the pixel shader.
- **Video is by far the most expensive type.**
  - The Media Foundation pipeline adds ~45 MB and ~10 threads.
  - `TransferVideoFrame` color-converts and scales every frame to the full back buffer.
  - It is still small next to any browser tab, and it pauses like everything else.
- The settings process uses ~18 MB while open and 0 when closed.
- Exe size: ~90 KB, everything included. The engine-only v1 was 16 KB.

## Misc

- `IShellItemImageFactory::GetImage` gives Explorer's cached thumbnail for videos. It is a cheap way to preview MP4s without starting Media Foundation in the UI.
- The accent color comes from `HKCU\...\Explorer\Accent\AccentPalette`. Windows itself uses index 1 (lighter) on dark backgrounds and index 4 (darker) on light backgrounds.
- `DWMWA_USE_IMMERSIVE_DARK_MODE` (20) plus `DWMWA_CAPTION_COLOR` (35) give a caption bar that matches the custom-drawn client area.
- Dark tray menus: uxtheme ordinals 135 (`SetPreferredAppMode(AllowDark)`) and 136 (`FlushMenuThemes`). Undocumented but widely used.
- `config.ini` is created with a UTF-16 BOM so that `WritePrivateProfileStringW` stores Unicode paths correctly.
- Other processes (indexer or antivirus) briefly lock `config.ini` now and then. Writers outside the app must retry.
