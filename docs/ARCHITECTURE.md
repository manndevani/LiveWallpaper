# Architecture

## One executable, several roles

| Command | Role | Lifetime |
|---|---|---|
| `livewall.exe` | **Engine**: wallpaper window, tray icon | always running; started at sign-in by Task Scheduler |
| `livewall.exe -settings` | **Settings window** | only while open |
| `livewall.exe -add` | Settings window, opening the file picker | only while open |
| `livewall.exe -quit` | Stops the engine and waits until it has exited | instant |
| `livewall.exe -snapshot <id> <out.png> [sec]` | Renders one frame offscreen (testing) | instant |

The UI lives in a separate process so the engine never pays for Direct2D, DirectWrite, the shell or thumbnails.
Closing the settings window returns all of that memory.
Running `livewall.exe` while the engine is already up opens settings instead of starting a second engine.

```
           config.ini (UTF-16, %APPDATA%\livewall)   <- single source of truth
             ^ write                     ^ read
             |                           |
  +----------+---------+  livewall.reload  +--+-------------------------+
  |  settings process  | ----------------> |  engine process            |
  |  settings.cpp      |  livewall.status  |  engine.c  tray, timers    |
  |  D2D + DirectWrite | <---------------- |  desktop.c WorkerW/Progman |
  |  preview.c thumbs  |  livewall.pause   |  gfx.c     D3D11 swapchain |
  +--------------------+  WM_CLOSE (stop)  |  src_*.c   sources         |
                                           +----------------------------+
```

IPC uses registered window messages to the engine's hidden window (class `livewall_engine`); see `ipc.c`.
`livewall.status` returns a packed `LRESULT`: status in bits 0–7, fps in 8–23, width in 24–39 and height in 40–55.

## Modules (`src/`)

| File | Responsibility |
|---|---|
| `version.h`, `livewall.rc` | Version number and the exe's file details |
| `main.c` | `entry()`: argument parsing and role dispatch, per-monitor DPI v2 |
| `util.c` | CRT replacements (`memset/memcpy`, strings), app dir, log file, drawn app icon, dark popup menus |
| `config.c/.h` | `Config` load/save, built-in list, `kind_of()` by extension, library add/remove |
| `ipc.c/.h` | Finding, querying, reloading, pausing, starting and stopping the engine from another process |
| `autostart.c/.h` | Per-user logon task through `schtasks /Create /XML` |
| `desktop.c/.h` | Getting behind the icons (both Explorer layouts); "is the desktop fully covered"; "is the cursor over the desktop" |
| `gfx.c/.h` | D3D11 device on the chosen adapter, swap chain, constants, fullscreen draw, brightness dim pass |
| `source.h` | `Source` vtable: `open / close / draw / play / failed / park` + `SourceInfo` |
| `src_shader.c` | Built-in shaders (bytecode embedded at build time); custom `.hlsl` files compiled at load with `d3dcompiler_47.dll`, which is freed right after |
| `src_video.c` | `IMFMediaEngine` + DXGI device manager; `TransferVideoFrame` straight into the back buffer |
| `src_image.c` | WIC still images (one immutable texture) and animated GIFs (CPU compositing into a dynamic texture) |
| `still.c/.h` | Hands still pictures to the Windows wallpaper (zero-cost mode) |
| `preview.c/.h` | Offscreen rendering through the same sources: settings thumbnails and `-snapshot` |
| `engine.c/.h` | Message loop, tray menu, scheduling, pause logic, recovery |
| `settings.cpp` | Custom-drawn settings window |
| `shaders/` | HLSL. `fullscreen.hlsl` (VS), `blit` (textures + fit), `dim` (brightness), one file per built-in |

## Engine loop

`MsgWaitForMultipleObjectsEx` waits on a high-resolution waitable timer, and the timer is armed only while something animates. A still image draws once, and then the engine just sleeps.

- **Frame tick** (`fps` from the config; 60 for video, which presents only when a new frame arrives) → `render()`.
- **Poll** (500 ms, coalescable timer): attach or re-attach to the desktop, fix the z-order, check whether the desktop is covered, resize, check the source for failure, update the fps counter.
- **Pause reasons** (any one stops the frame timer and video playback):
  - user pause;
  - desktop fully covered;
  - session locked;
  - display off;
  - on battery.
  The last frame stays on screen.
- **Idle memory:**
  - 2 s after drawing stops, the working set is handed back (`trim_if_idle`), and again every 60 s while idle.
  - After 10 s, a source with `park()` (video) frees its decoder. On resume it reopens at the same position.
- **Still pictures** never get a device: `still.c` hands them to Windows (`IDesktopWallpaper`, with a dimmed copy when brightness < 100%). The engine then keeps only its tray icon. If Windows refuses, the engine draws the picture itself.
- **Device loss / GPU switch** → `recover()` rebuilds the device and reopens the source.
- The process runs at BELOW_NORMAL priority with EcoQoS (power throttling) on.

## Rendering

- One swap chain covers the whole virtual desktop: FLIP_DISCARD with `DXGI_SCALING_STRETCH`.
- Shaders render at half resolution by default; DWM upscales for free.
- Video and images always render at full resolution.
- `FrameConsts` (64 bytes) carries:
  - resolution;
  - `phase` (loops every 120 s) and `time`;
  - mouse, click and click age;
  - dim;
  - the fit transform.
- Brightness is a second fullscreen pass with blend `dst × (1 − a)`.
- Fit (cover/contain/stretch) is a uv scale and offset computed by `gfx_fit`.

## Start at sign-in

`autostart.c` registers a Task Scheduler task named `livewall`:
- logon trigger for the current user;
- least privilege and interactive token;
- no time limit;
- allowed on battery;
- priority 4.

Logon tasks start before Explorer runs its staggered Run-key and Startup-folder apps, so the wallpaper appears early. The engine waits in state *Waiting for desktop* until Explorer's `Progman` window exists.
If the exe moves, the next manual start re-registers the task with the new path.
