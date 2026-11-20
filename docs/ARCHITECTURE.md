# Architecture

## One executable, several roles

| Command | Role | Lifetime |
|---|---|---|
| `livewall.exe` | **Engine**: wallpaper window, tray icon | always running |
| `livewall.exe -startup` | Engine, started by the sign-in task: never opens a window | always running |
| `livewall.exe -settings` | **Settings window** | only while open |
| `livewall.exe -add` | Settings window, opening the file picker | only while open |
| `livewall.exe -quit` | Stops the engine and waits until it has exited | instant |
| `livewall.exe -snapshot <id> <out.png> [sec]` | Renders one frame offscreen (testing) | instant |
| `livewall.scr /s`, `/p <hwnd>`, `/c` | **Screen saver** (`saver.c`): full screen, the preview in Windows' dialog, or settings on the Lock screen tab | while the PC is idle |

The UI lives in a separate process so the engine never pays for Direct2D, DirectWrite, the shell or thumbnails.
Closing the settings window returns all of that memory. While it is open it hands back what it no longer touches (after 3 s without input, or when it goes to the background): ~3–4 MB instead of ~18.
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
| `lock.c/.h` | Turning livewall's screen saver on and off in Windows, keeping the `.scr` copy up to date |
| `saver.c` | The screen saver window and its draw loop (same sources as the engine) |
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
- **Memory:**
  - 5 s after drawing starts, the working set is handed back once (`trim_memory`). Most of a fresh device's memory is only touched during setup; the per-frame set is ~2–3 MB.
  - 2 s after drawing stops, it is handed back again, and again every 60 s while idle.
  - After 10 s, a source with `park()` (video) frees its decoder. On resume it reopens at the same position.
- **Still pictures** never get a device: `still.c` hands them to Windows (`IDesktopWallpaper`, with a dimmed copy when brightness < 100%). The engine then keeps only its tray icon. If Windows refuses, the engine draws the picture itself.
- **Device loss / GPU switch** → `recover()` rebuilds the device and reopens the source.
- **No GPU driver** (some virtual machines, a missing or broken driver) → `gfx_init` falls back to Direct3D's CPU renderer (WARP). `gfx.software` is set (also for the "Microsoft Basic Render Driver"), and Auto resolution then aims at 640×360 instead of 2560×1600. Build with `-DLW_FORCE_WARP=1` to test it on a normal PC.
- The process runs at BELOW_NORMAL priority with EcoQoS (power throttling) on.

## Rendering

- One swap chain covers the whole virtual desktop: FLIP_DISCARD with `DXGI_SCALING_STRETCH`.
- Shader resolution is chosen from the display by default (`resolution=0`, Auto): 1:1 up to 2560×1600 pixels, and 1/2 or less on bigger desktops (4K, wide multi-monitor), where DWM upscales for free. Full (`1`) and Half (`2`) can be picked in settings.
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
The task runs `livewall.exe -startup`. With `-startup` the engine never opens a window, not even on a first run, and a second copy started that way exits silently. Without it (the user opening the exe), a first run opens settings and a second copy opens settings too.

The config stores the full task command (`autostart_exe`). If it differs from "<this exe> -startup" (the exe moved, or the task predates `-startup`), the next start re-registers the task.

## Lock screen (screen saver, since 2026-11-20)

livewall never draws its own lock screen and never checks passwords. It plugs into what Windows already has:

- **Screen saver.** Turning on *Show when idle* copies the exe to `%LOCALAPPDATA%\livewall\livewall.scr` and sets `SCRNSAVE.EXE`, the timeout and `ScreenSaverIsSecure` ("Ask for password") in `HKCU\Control Panel\Desktop` through `SystemParametersInfo`. Windows starts it when the PC is idle. When it closes and *Ask for password* is on, Windows locks the PC and its own sign-in screen asks for the password. *Ask for password* is on by default.
- **These settings live in Windows, not `config.ini`**, because Windows is what reads them. The settings window reads them back every time it is activated, so changes made in Windows' own Screen Saver Settings show up.
- The `.scr` is a copy (Windows needs a fixed path). The engine refreshes it at start if it differs from the running exe (size or date), so updates reach it.
- **Which wallpaper:** `[lockscreen] wallpaper=` in `config.ini`, empty for "same as desktop".
- **Matching lock screen picture** (optional, `match_picture=1`): the settings window renders the lock wallpaper at screen size to `%APPDATA%\livewall\lockscreen.png` and hands it to `Windows.System.UserProfile.LockScreen.SetImageFileAsync` (WinRT ABI from C++, no C++/WinRT). So the screen saver and the sign-in screen behind it look alike.
- The saver draws like the engine: the same sources, speed, brightness and frame cap; one still frame on battery when *Pause on battery* is on; no frames while the display is off. It trims its memory once after 5 s (35 MB to 2.5 MB). Any key, click, or mouse movement over 10 px ends it (movement in the first 0.6 s is ignored).
- The engine needs nothing special: the saver's topmost full-screen window covers the desktop, so the engine pauses as it does for any full-screen app.
- **Lock shortcut, Ctrl+Alt+L** (`[lockscreen] shortcut=1`, default on): the engine holds it with `RegisterHotKey` (no keyboard hook, no polling). `lock_now()` sends `WM_SYSCOMMAND / SC_SCREENSAVE` to the desktop window, so Windows starts its configured screen saver exactly as after the idle time and stays in charge of locking when it ends. Every 500 ms `lock_check()` looks for it (`SPI_GETSCREENSAVERRUNNING` or the saver window, class `livewall_saver`); if neither shows up within 5 s, it calls `LockWorkStation()`. If livewall is not the screen saver, or *Ask for password* is off, the shortcut calls `LockWorkStation()` at once. So the shortcut always ends in a locked PC. The saver ignores key repeats and keys in its first 0.6 s, so the held Ctrl and Alt don't close it.
- Windows+L can't be taken over: it is handled by Windows itself, and the only way around that (turning off Windows' own lock and catching the keys) would weaken the PC's lock, so it is not done.

## Linux (`linux/`, since 2026-11-20)

A separate, small C program (~80 KB) with the same ideas as the Windows engine. It doesn't share code with `src/`, only the config format and the shaders.

| File | Responsibility |
|---|---|
| `main.c` | Commands (`-set`, `-list`, `-quit`, `-pause`, `-snapshot`…), the engine loop (`poll()` on the display, a timerfd for frames, a signalfd, libmpv's wake-up eventfd), EGL setup, battery check, XDG autostart entry |
| `config.c` | `~/.config/livewall/config.ini` (same sections and keys as Windows; `autostart` instead of `start_with_windows`). Written atomically (temp file + rename) |
| `gl.c` | OpenGL ES 2.0: builds shaders as `prelude.glsl` + body + `epilogue.glsl`; Shadertoy-style custom files get a small adapter. Lower resolution renders into a texture that is then stretched. Brightness is a black quad with alpha |
| `media.c` | Videos, GIFs, pictures through libmpv's render API, loaded with `dlopen` (optional). `hwdec=auto-safe`, `profile=fast`, no audio, small demuxer cache |
| `wayland.c` | One wlr-layer-shell surface per monitor on the *background* layer. Draws only after the compositor's frame callback, so it stops by itself when hidden |
| `x11.c` | One `_NET_WM_WINDOW_TYPE_DESKTOP` window covering the screen, kept below. "Covered" = the active window is fullscreen or maximized |
| `png.c` | Tiny PNG writer for `-snapshot` |

- The shaders live in `shaders/glsl/` and are GLSL ES 1.00 twins of `src/shaders/*.hlsl`. `pos` is in pixels with y down, like Direct3D, so the two versions match line for line. Android and macOS will use the same files.
- Commands talk to the running engine with signals: they write `config.ini` and send `SIGHUP` (reload), `SIGUSR1` (pause), `SIGUSR2` (resume) or `SIGTERM` (quit). The pid file is `$XDG_RUNTIME_DIR/livewall.pid`, held with `flock`.
- `make -C linux` needs the dev packages listed in `docs/LINUX.md`. The layer-shell protocol XML is kept in `linux/protocols/` (from the wlroots project, permissive license).
