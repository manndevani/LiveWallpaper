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
| Shader, rendering | 14.3 MB | 16 | at the time; see "Memory while drawing" below (now ~2–3 MB) |
| Video, rendering | 59–68 MB | 26 | at the time; now ~14 MB (see below) |

- "Private WS" is what Task Manager shows as Memory. Trimming (`SetProcessWorkingSetSize(-1, -1)` plus `HeapCompact`) moves pages to the standby list. They come back as soft faults when drawing resumes, which is cheap. Committed memory does not shrink. Freeing the decoder (video) and handing pictures to Windows does shrink it.
- An earlier note here said that less than ~10 MB while *rendering* was not reachable, because the GPU driver alone is that big. That was wrong: most of the driver memory is touched only once; see "Memory while drawing" below.

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

## Memory while drawing (2026-11-20, same machine, 1920×1080, full-res shaders)

**Where the 14 MB went.** `tools\wsdetail.ps1` walks the process with `VirtualQueryEx` and `QueryWorkingSetEx` and groups the private resident pages by owner:

| Owner | NVIDIA (default) | Intel (`integrated_gpu=1`) |
|---|---|---|
| Private allocations (driver and D3D heaps) | 9.9 MB | 10.3 MB |
| `nvwgf2umx.dll` + `nvgpucomp64.dll` (NVIDIA driver and shader compiler data) | 2.5 MB | – |
| `igc64.dll` (Intel shader compiler) | 0.7 MB | 0.7 MB |
| Everything else, livewall.exe included | < 1 MB | < 1 MB |
| **Total** | 14.8 MB | 12.4 MB |

livewall's own code and data are a rounding error. It's all the graphics driver, and most of that is **touched once**: creating the device and compiling shaders. Measured by emptying the working set once, from outside, while it kept drawing:

| | Before | Steady state after one trim |
|---|---|---|
| NVIDIA | 14.5 MB, 16 threads | **3.0 MB**, stays there for minutes |
| Intel | 12.1 MB, 6 threads | **1.6 MB** |

Page faults afterwards: ~1/s on average (0 most of the time). The per-frame set is small, so nothing thrashes.

So the engine now trims once **5 s after drawing starts** (`trim_memory` in engine.c). It's the same call it already used while paused. Results with the engine doing it itself:

| Wallpaper | NVIDIA | Intel |
|---|---|---|
| Built-in shaders | 2.0 MB at 25 s, ~3.0 MB plateau from 60 s to 3 min | 1.0–1.1 MB |
| GIF 320×180 | 3.2 MB | 2.2 MB |
| MP4 640×360 | 34–38 MB (see below) | 10.8 MB |

**Video.** Decoding does run on the GPU (the `videodecode` engine is busy). The 36 MB is Media Foundation's frame queue.
- `MF_MEDIA_ENGINE_REAL_TIME_MODE` (low-latency mode, fewer buffered frames) brings it to **14 MB** at the same 29–30 fps.
- `MF_MEDIA_ENGINE_FORCEMUTE` saves only ~5 MB on its own and nothing on top of real-time mode, so it isn't used.

**Snapshot gotcha (fixed).** With real-time mode, the video size is known before the first frame is decoded. A forced `TransferVideoFrame` at that point copies black. `-snapshot` of a video now waits for a real frame (`OnVideoStreamTick == S_OK`).

**What trimming is and isn't.**
- Trimmed pages go to the standby/modified lists. Windows keeps them in RAM (compressed if needed) until something else wants the memory.
- Task Manager's Memory column (private working set) shows the real resident cost.
- Commit charge does not shrink.

**Picking the GPU matters too.** The integrated GPU is lighter in every way here: half the memory, 6 threads instead of 16, and ~55 ms/s GPU time instead of ~95 for the same shader.

## Software rendering (WARP) fallback (2026-11-20, same machine, forced)

| Wallpaper | Render size | Private WS | CPU (24 threads) | fps |
|---|---|---|---|---|
| Aurora | 640×360 (Auto on CPU) | 0.7 MB | 2.6–2.7 % (~60 % of one core) | 29 |
| Ripples | 640×360 | 0.7 MB | 0.7 % | – |
| GIF 320×180 | 1920×1080 | 1.6 MB | 1.3 % | – |
| MP4 640×360 | 1920×1080 | 25.5 MB | 0.5–0.7 % | 29 |

- The GPU Engine counters still show time for WARP processes (presentation and composition through DWM). It isn't comparable with hardware numbers.
- Video still plays on WARP: Media Foundation decodes on the CPU, and `TransferVideoFrame` works with a WARP device.
- `DXGI_ADAPTER_FLAG_SOFTWARE` identifies both WARP and the "Microsoft Basic Render Driver" (what Windows uses with no display driver installed).

## Packaged-app AppData redirection (2026-11-20)

- When livewall is launched from inside a packaged (MSIX) app, such as a terminal hosted by a Store-packaged app, Windows redirects its `%APPDATA%` writes to `%LOCALAPPDATA%\Packages\<app>\LocalCache\Roaming\`. That folder is invisible to Explorer and to the same exe launched normally.
- Result: `IDesktopWallpaper::SetWallpaper` on a file in our AppData fails with `0x80070002` (file not found). `still.c` retries with a copy in `%TEMP%`, which is not redirected.
- It also means the config used during development sessions is not the one the sign-in task will read. Confirmed 2026-11-20: after a reboot the real `%APPDATA%\livewall` had no config yet, so the sign-in start acted as a first run and opened the settings window. Since then the task runs `livewall.exe -startup`, which never opens a window.
- The redirection only applies to files the packaged process creates. Writing to a file that already exists in the real `%APPDATA%` changes the real file (2026-11-22: a `WritePrivateProfileString` from a packaged shell to `%APPDATA%\livewall\config.ini` changed the user's real config). Test with a config path in the scratchpad, never the AppData one.
- Testing from PowerShell: `FindWindowW("livewall_engine", $null)` finds nothing, because PowerShell passes `$null` to a string parameter as `""`, which asks for a window with an empty title. Use `[NullString]::Value`.

- "GPU time" is the sum of `\GPU Engine(pid_*)\Running Time` across all engines. For video that includes the decode and video-processor engines.
- Shader cost is almost the same whichever shader is used. Present and composition dominate, not the pixel shader.
- **Video is by far the most expensive type.**
  - The Media Foundation pipeline adds ~45 MB and ~10 threads.
  - `TransferVideoFrame` color-converts and scales every frame to the full back buffer. Since 2026-11-22 it skips the scaling for a video smaller than the screen; see below.
  - It is still small next to any browser tab, and it pauses like everything else.
- **Where video time goes on the graphics card** (2026-11-22, generated H.264 test files, 1920×1080, engine on the NVIDIA RTX A3000, which drives the screen; earlier versions of this note said Intel, which was wrong). Split by engine type from `\GPU Engine(pid_*_engtype_*)\Running Time`:
  - The `videodecode` engine takes 230–600 ms per second, and the `3d` engine (colour conversion, scaling, present) takes 90–150 ms per second.
  - Decoding is the larger part, and only the file decides it. The numbers jump between runs because the card changes its own clock, so compare builds in alternating runs.
  - 720p drawn into a 1280×720 buffer instead of a 1920×1080 one: 3d time 133–135 → 79–85 ms/s.
  - A 60 fps video held to a 30 fps cap: 3d time 87–94 → 39 ms/s, CPU 0.94% → 0.70%. Skipping a frame after `OnVideoStreamTick` returned `S_OK` is fine; the next call reports the next new frame.
  - A 640×480 or 720p test video encoded with the MF Sink Writer is a quick C program (`MFCreateSinkWriterFromURL`, RGB32 in, H.264 out). In C, `MFSetAttributeSize` and `MFSetAttributeRatio` don't exist (they are C++ inline helpers); set `MF_MT_FRAME_SIZE` and the ratios with `SetUINT64((hi << 32) | lo)`.
- The settings process used ~18 MB while open, and 0 once closed. Since 2026-11-20 it hands memory back once thumbnails are built and the window has had no input for 3 s, or as soon as it goes to the background. It is ~19 MB for the first second or two, then ~3–4 MB while open (sampled every second with the window in front). Drawing it with Direct2D's software renderer instead was worse: it held ~9.6 MB, because it keeps and redraws a full CPU copy of the window every second (the fps readout).
- Exe size: ~114 KB, everything included (~94 KB before the Explorer icon was added on 2026-11-20; the icon is PNG entries 16–128 px, about 20 KB, and the 256 px size alone would have added another ~26 KB). The engine-only v1 was 16 KB.

## Misc

- `IShellItemImageFactory::GetImage` gives Explorer's cached thumbnail for videos. It is a cheap way to preview MP4s without starting Media Foundation in the UI.
- The accent color comes from `HKCU\...\Explorer\Accent\AccentPalette`. Windows itself uses index 1 (lighter) on dark backgrounds and index 4 (darker) on light backgrounds.
- `DWMWA_USE_IMMERSIVE_DARK_MODE` (20) plus `DWMWA_CAPTION_COLOR` (35) give a caption bar that matches the custom-drawn client area.
- Dark tray menus: uxtheme ordinals 135 (`SetPreferredAppMode(AllowDark)`) and 136 (`FlushMenuThemes`). Undocumented but widely used.
- `config.ini` is created with a UTF-16 BOM so that `WritePrivateProfileStringW` stores Unicode paths correctly.
- Other processes (indexer or antivirus) briefly lock `config.ini` now and then. Writers outside the app must retry.
- Animated README preview (2026-11-20): `-snapshot` now moves a pretend cursor over time (it clicks every 3 s), so frame series of Ripples and Glow Grid show them reacting. WIC's own GIF palette (`IWICPalette::InitializeFromBitmap`) bands these dark gradients badly without dithering, and error diffusion makes the GIF ~2× bigger. A median-cut palette per frame plus a 4×4 ordered dither looked smooth: 480×270, 10 fps, 134 frames = 4.9 MB.

## Safety review (2026-11-22, same Windows 11 machine)

- **DLL search order.** Without the C runtime there is no load config table, so `/DEPENDENTLOADFLAG` alone does nothing (the linker warns LNK4266). Defining `_load_config_used` by hand with `DependentLoadFlags = LOAD_LIBRARY_SEARCH_SYSTEM32` makes it work. Before: 7 of 8 fake DLLs next to the exe were loaded (a broken one shows a "Bad Image" box to the user; suppress it in tests with `SetErrorMode`).
- **Private bytes vs. working set.** A fresh engine shows ~102 MB private bytes but a 0.9 MB working set. The private bytes are almost all memory the graphics driver commits for the device and swap chain; it isn't touched. Report the working set as "RAM in use".
- **Media Foundation stays loaded** after a video (MFPlat and the media engine DLLs stay in the process). Their pages can still be trimmed: the working set fell from 18.1 MB to 1.1 MB once a switch re-armed the trim.
- **Clean config for test processes:** set `APPDATA` to an empty folder before starting a test exe; it then makes its own `config.ini` there. Put `start_with_windows=0` in it before starting a test *engine*, or it re-registers the startup task to point at the test exe. Its `livewall.reload` still reaches the user's real engine, which just re-reads the real config.
- **Timing shaders on the graphics card.** D3D11 timestamp queries cost nothing and never stall: a DISJOINT query around two TIMESTAMPs, in a ring of 4, read with `D3D11_ASYNC_GETDATA_DONOTFLUSH`. Aurora takes 0.06 ms per frame at 1920×1080.
- **The same frame measures slower when the card is lightly loaded**, because the card lowers its own clock.
  - A test shader read 3.5 ms per frame, then 9.6 ms a few seconds later, with nothing else changed.
  - So a limit of 20% busy also lowered shaders that were fine.
  - At 50% busy for 5 seconds, only a really heavy shader trips it: 98% busy did, 27% didn't.
- **A test engine started with `Start-Process` can outlive `Stop-Process $p`**. Stop it by name and wait. If the real engine starts while a test engine still has its window, the real one quits at once (only one engine runs at a time), and no wallpaper is left running.
- From PowerShell, `cmd /c build.bat` can fail with "not recognized" even after `cd`: cmd starts in the process's current folder, not PowerShell's location. Use the full path.
- **A packaged shell also *reads* a redirected copy.** Once a packaged process has created `%APPDATA%\livewall\livewall.log`, that shell sees its own copy in `%LOCALAPPDATA%\Packages\<app>\LocalCache\Roaming\` from then on. It looked as if the real engine had stopped logging. Ask the engine itself (`livewall.status`) instead of reading its log from such a shell.

## Sandboxed video helper (2026-11-22, same machine, 1920×1080, engine and helper on the NVIDIA RTX A3000 (LUID 0x1438c; this heading said Intel at first, which was wrong))

- **AppContainer start-up.**
  - A process can't be started in an AppContainer that is only derived from a name (`DeriveAppContainerSidFromAppContainerName`). It has to be registered once with `CreateAppContainerProfile`, which makes `%LOCALAPPDATA%\Packages\livewall.decoder`.
  - The exe did not need an extra permission, even when started from a folder in the user's profile (Temp). `sandbox.c` still grants the container read access to livewall.exe if `CreateProcess` ever says "access denied".
- **Handles instead of names.** Handles given to the helper (inherited through `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`, or duplicated into it) work even though the AppContainer couldn't open the same objects by name. This covers the file, the shared memory, the event, and the texture's NT handle.
- **Media Foundation in an AppContainer.**
  - H.264 decodes on the GPU inside the container.
  - The media engine reads the file through `MFCreateMFByteStreamOnStream` over a hand-written `IStream` on the file handle. The URL passed to `SetSourceFromByteStream` only needs the right extension.
  - A broken file fails with `0xC00D36C4` (unsupported byte stream).
- **The keyed mutex costs copy-engine time.**
  - Passing the shared texture between the processes keeps the GPU's copy engine busy for 80–110 ms per second at 30 fps (1080p texture), counted against the engine.
  - This happens even when the engine takes the frame and draws nothing, so it is the hand-over itself, not our copy.
  - Drawing the frame with a shader instead of `CopyResource` cost the same, so the plain copy stays.
- **Cost, helper against decoding in the engine** (two runs each, alternating builds):

| Video | Memory in use (engine + helper) | CPU | GPU 3d + copy (ms/s) |
|---|---|---|---|
| 1080p 30 fps, in the engine | 17.5–20.4 MB | 0.5% | 150 + 1 |
| 1080p 30 fps, helper | 1.8 + 10.5 MB | 0.7% | 70–78 + 80–90 |
| 4K 30 fps, in the engine | 69.5 MB | 0.55% | 57 + 0 |
| 4K 30 fps, helper | 1.6 + 22.0 MB | 0.85% | 32 + 26 |
| 4K 60 fps (cap 30), in the engine | 45.1 MB | 0.9% | 34 + 0 |
| 4K 60 fps (cap 30), helper | 1.7 + 9.4 MB | 1.4% | 15 + 18 |

- The helper hands its memory back 5 s into playing, as the engine does: from 44.5 MB to ~10 MB.
- **4K 60 fps keeps the RTX A3000's video decoder 82–86% busy** (the `videodecode` engine), with or without the helper. 4K 30 fps takes ~50%. Not yet measured on the Intel UHD (`integrated_gpu=1`).

## Where memory still goes (2026-11-23, same machine, 1920×1080)

Measured after the 5 s trim. "Graphics memory" is `\GPU Process Memory(pid_*)\Dedicated Usage` and `Shared Usage`, per chip. Task Manager's Memory column doesn't include it. On an integrated chip the shared part is ordinary RAM.

| | Private WS | Commit | Graphics memory |
|---|---|---|---|
| Engine, shader, default (NVIDIA) | 1.8 MB | 103 MB | 25 card + 17 shared (NVIDIA) + 19 shared (Intel) |
| Engine, any wallpaper, `integrated_gpu=1` | 0.8 MB | 27 MB | 33 shared (Intel), nothing on NVIDIA; 6 threads instead of 16 |
| Helper, 1080p 30 fps (NVIDIA) | 10.3 MB | 92 MB | 84 card + 22 shared |
| Helper, 4K 30 fps (NVIDIA) | 9–21 MB | 158 MB | **292 card + 62 shared** |
| Helper, 4K 30 fps, `integrated_gpu=1` | 18 MB | 146 MB | **343 shared = RAM** |

- **The engine is at the floor.** A ten-line test program with only a D3D11 device on the NVIDIA adapter and one 1920×1080 flip swap chain shows the same 25.2 + 16.7 + 18.7 MB. The 18.7 MB on the Intel chip, and the Intel driver DLL being loaded, come from creating *any* swap chain on this hybrid laptop. An HWND swap chain and a DirectComposition one behave the same. This is DXGI's doing, not livewall's.
- **The video decoder's frames are the big part.** Decoding the same files with a source reader (`MF_SOURCE_READER_D3D_MANAGER`, NV12 out, one frame held at a time) instead of the media engine:

| | Media engine (now) | Source reader | + `MF_LOW_LATENCY` |
|---|---|---|---|
| 4K 30 fps | 292 + 62 MB | 140 + 142 MB | **128 + 21 MB** |
| 1080p 30 fps | 84 + 22 MB | 43 + 37 MB | **40 + 18 MB** |

  - The decoder's surfaces come back as one texture array (one distinct texture).
  - Setting `MF_SA_MINIMUM_OUTPUT_SAMPLE_COUNT` to 4 on the decoder's output changed nothing.
  - `MF_LOW_LATENCY` on the reader is what removes the shared part.
  - Telling the media engine to output NV12 instead of BGRA (`MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT`) changed nothing, to the byte. The extra ~150 MB is inside the media engine, and no attribute reaches it.
  - So a helper built on the source reader plus `ID3D11VideoProcessor` (for colour conversion, scaling and the fit) would save roughly 200 MB of graphics memory per 4K video and ~45 MB per 1080p one. The media engine does several things for free that the source reader would leave to us: pacing, looping, playback speed, rotation of phone videos (`MF_MT_VIDEO_ROTATION`), format changes mid-stream, padded surfaces (display aperture), and 10-bit HDR (P010).

## The source-reader player (2026-11-23, same machine)

The helper now decodes with the source reader and converts with the graphics card's video processor; the media engine is kept as a fallback (decoder.c). Measured after the 5 s trim, the old player forced with a test build (`-DLW_FORCE_MEDIA_ENGINE=1`):

| Helper | Media engine (old) | Source reader (new) |
|---|---|---|
| 4K 60 fps, NVIDIA | 292 card + 62 shared MB, commit 150–160 MB | **147 card + 38–98 shared MB**, commit 240–267 MB |
| 1080p 30 fps, NVIDIA | 84 card + 22 shared MB, commit 90 MB | **48 card + 20 shared MB**, commit 113 MB |
| 4K 60 fps, `integrated_gpu=1` | 343 MB shared (= RAM) | **178 MB shared** |
| 1080p 30 fps, `integrated_gpu=1` | 119 MB shared | **73 MB shared** |
| 720p 30 fps, CPU | 1.85% | **0.49%** |

- **Graphics memory roughly halves.** On the integrated chip that is ordinary RAM: 165 MB less for a 4K video.
- **Commit goes up, working set doesn't.** The source reader reserves ~90 MB more at 4K (~20 MB at 1080p) that it barely touches: the private working set stays 5–20 MB. The shared part moves up and down in steps of ~12 MB (one 4K NV12 frame) while playing; it does not grow over 45 s. No copies to the CPU: the copy engine stays idle and CPU use is lower than before.
- **`MF_LOW_LATENCY` matters:** without it the same 4K video takes 160 card + 134 shared MB and 350 MB commit.
- **Picture checks** (`-snapshot` of ffmpeg-made test files against the old player): turned phone videos (90°, 180°, 270°), non-square pixels (720×576 at 16:11) and full-range colour come out the same. `MF_MT_VIDEO_ROTATION` is the counter-clockwise turn already applied to the content; turning it back clockwise with `D3D11_VIDEO_PROCESSOR_ROTATION` by the same amount is right. The video processor's source rectangle is in the decoded frame, before the turn.
- A standard-definition file tagged BT.601 is now shown with BT.601 colours; the media engine used the HD (BT.709) formula on it (green 0,216,0 instead of 0,255,0). Untagged files use BT.709 from 720 lines up and BT.601 below, as players do.
- 10-bit HEVC (P010 out) and VP9 in WebM play on the new path inside the sandbox on this machine.
- **The media engine and B-frames:** for a moment after starting or after a jump, `TransferVideoFrame` succeeds but hands over a blank (white) picture on files with B-frames (almost every encoder makes them; the earlier test videos from the MF sink writer had none). The sandboxed helper drew right after a jump, so `-snapshot` of such files was white. Fixed by drawing only when `OnVideoStreamTick` reports a new frame while playing, and by keeping on asking for frames during a jump (the media engine finishes a jump on such files only while it is asked).
- The fallback takes over by itself where the new path can't work: on a software device (`-DLW_FORCE_WARP=1`) there is no video device (E_NOINTERFACE), and the media engine plays at 30 fps. The engine logs which player is used when it isn't the new one.

## Linux port (2026-11-20, tested in WSL2 Ubuntu 26.04, Mesa llvmpipe, no GPU)

- **Same picture as Windows.** `-snapshot` of Aurora and Dunes at 9 s on Linux and on Windows look identical by eye. The GLSL versions use `mod(float)` for HLSL's integer `%`, and y-down pixel positions (`res.y - gl_FragCoord.y`).
- **Headless test setup:** Sway with `WLR_BACKENDS=headless WLR_RENDERER=pixman`, screenshots with `grim`; X11 with `Xvfb` and `xwd`. A fullscreen `foot` window covers the desktop.
- **Pausing on Wayland needs no code.** Drawing waits for the compositor's frame callback. Sway sends none while a fullscreen window hides the background: 1 CPU tick in 3–5 s while covered, against ~250 per 3 s drawing on the CPU. It resumed as soon as the window closed.
- `-pause`/`-resume`, `-set` while running (switching shader → video), and `-quit` all verified under Sway; X11 drawing, mouse (Glow Grid follows the pointer) and `-quit` verified under Xvfb.
- The CPU numbers above are software rendering (llvmpipe). They say nothing about cost on a real GPU, which is not measured yet. Video under llvmpipe is very expensive (mpv's shaders run on the CPU), and RSS was ~330 MB; `profile=fast` was added afterwards.
- WSL quirk: commands passed through `wsl -- sh -c "…"` from PowerShell lose `$variables`. Put test steps in a script file and run `wsl -- bash file.sh`.
- Ubuntu has no package with `wlr-layer-shell-unstable-v1.xml`; it was taken from gitlab.freedesktop.org/wlroots/wlr-protocols.

## Screen saver (2026-11-20, same Windows 11 machine)

- A screen saver is any exe Windows starts with `/s` (full screen), `/p <hwnd>` (draw inside the preview box of Screen Saver Settings) or `/c` (configure; also no arguments when the file is a `.scr`). Some callers write `/p:1234` or `/c:1234`, so both forms are parsed.
- Windows only checks "Ask for password" (`ScreenSaverIsSecure`) as the screen saver ends: it locks the session and shows its own sign-in screen. The screen saver never needs to handle passwords.
- `SPI_GETSCREENSAVEACTIVE` reports TRUE even with "(None)" chosen, so whether a screen saver is really on also needs `SCRNSAVE.EXE` from `HKCU\Control Panel\Desktop`.
- Policy keys under `HKCU\Software\Policies\Microsoft\Windows\Control Panel\Desktop` override the user's choice. The settings window shows a note instead of switches when that key has values.
- Like `scrnsave.lib`, the full-screen window closes when the app loses activation. Started by hand from a script while another window was taking focus, it sometimes closed at once; started from the settings window (Preview) it stays until real input.
- With the same 5 s trim as the engine, the full-screen saver drops from 35.5 MB to 2.5 MB. Aurora at 1920×1080 used 62 ms of CPU over 5 s.
- Testing tip: in PowerShell, `$null` passed to a `string` P/Invoke parameter becomes `""`, so `FindWindow("class", $null)` finds nothing. Declare the title as `IntPtr` and pass `[IntPtr]::Zero`.
- Windows+L is handled by Windows itself: `RegisterHotKey` can't take it and keyboard hooks don't see it. The only way to reuse it is the `DisableLockWorkstation` policy plus a hook, which turns off Windows' own lock, so livewall uses Ctrl+Alt+L instead.
- `WM_SYSCOMMAND / SC_SCREENSAVE` sent to `GetDesktopWindow()` asks Windows to start the configured screen saver now, so it is handled like an idle start (including "Ask for password"). Windows does not ask for the password if the screen saver ends within `ScreenSaverGracePeriod` (5 s by default) of starting.
- Reading the real log from a session whose `%APPDATA%` is redirected (see "Packaged-app AppData redirection"): `\localhost\c$\Users\<name>\AppData\Roaming\livewall\livewall.log` is not redirected; `C:\...` and `\?\C:\...` are.
- First real test of Ctrl+Alt+L (2026-11-20): after `SC_SCREENSAVE`, `SPI_GETSCREENSAVERRUNNING` was still FALSE 1.5 s later, so the safety check locked the PC under the screen saver. The check now also looks for the saver's window and waits up to 5 s, and the log records when it showed.
- Confirmed by the user (2026-11-20): with "Ask for password" on, a screen saver started through `SC_SCREENSAVE` and ended after 2–3 s went back to the desktop with no password. That is the 5 s grace period, which also applies to `SC_SCREENSAVE` starts. Its value isn't in `HKCU\Control Panel\Desktop` by default (an `HKLM\...\Winlogon` value can change it).
- `log_line` uses static buffers, so it must not be called from thread-pool callbacks. The engine logs `PC locked` / `PC unlocked` from `WM_WTSSESSION_CHANGE` instead.
- When Windows starts the screen saver (idle or `SC_SCREENSAVE`), `FindWindow` from the engine does not find the saver's window, although it does when `livewall.exe /s` is run by hand. Most likely Windows runs it on its own screen-saver desktop. The process is still visible: Toolhelp lists it as `livewall.scr`.
- `LockWorkStation()` returns at once, but Windows reports `WTS_SESSION_LOCK` and shows the lock screen about 1 s later (log, 2026-11-20), the same as Windows+L. A screen saver that closes right after calling it shows the desktop during that second.
- A password-protected screen saver started by Windows runs on its own "Screen-saver" desktop, and Windows treats the PC as locked while it runs (Microsoft's old "Security and Screen Savers" note; confirmed by `FindWindow` failing from the engine). With `ScreenSaverGracePeriod` = 0 (HKLM `...\Winlogon`, admin), Windows goes from that desktop straight to the lock screen on input, without livewall's help. Confirmed by the user (2026-11-20): with it set to 0, Ctrl+Alt+L then a mouse move went straight to the PIN screen, with no wait.
- Calling `LockWorkStation()` from inside a screen saver started by Windows (2026-11-20): Windows took 1–2 s to report the lock, and the user saw the desktop on the way, even with the saver held on screen until `WTS_SESSION_LOCK`. Ending the screen saver after the grace period, so Windows locks by itself, is the clean path.
- Start-up after a power-on with fast startup (2026-11-22): the sign-in task started the engine at 17:56:45, but the wallpaper was only on the desktop at 17:56:50, so the user saw Windows' own wallpaper for about 5 s. The log didn't show whether the wait was for the graphics device, for Explorer's desktop (`Progman`), or the engine being slowed by below-normal priority plus EcoQoS while the PC was busy. Since then the engine starts at full speed, checks every 100 ms, and logs the timings ("graphics ready at", "desktop found at"). A warm restart takes about 0.35 s. Next cold start: read that line.
- First cold start with the timing log (2026-11-22, power-on at 18:06:28, sign-in 18:06:47): "on the desktop 4656 ms after start", with the graphics device and the desktop both found at the first check, 4313 ms in. So Explorer was not the hold-up: the engine's own start-up work came before the first check. Part of it was a bug: the "has the exe moved?" check copied the new task command into the config and then compared it with itself, so it never saved, and every sign-in re-registered the startup task through `schtasks.exe` (about 1 s on a busy PC). Fixed, and that check and the screen saver copy update no longer come before the wallpaper: they run once it is on screen. Warm start now: 250–300 ms, about 240 ms of it creating the Direct3D device.
- Cold start after the fix (2026-11-22, power-on, sign-in 18:26:25 → engine 18:26:26): "engine started (469 ms)", graphics ready at 2251 ms, on the desktop at 2953 ms, down from 4656 ms. What is left is mostly Windows loading the graphics driver for the first Direct3D device (about 1.8 s while the PC is busy signing in), then about 0.7 s for the window and the first frame. Explorer's desktop was ready by the first check.
