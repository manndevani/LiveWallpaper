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
| `livewall.exe -check <file> <out.png>` | The gallery's safety check: opens and draws a wallpaper file the way the engine does, saves the frame at 8 s, and writes `<out.png>.txt` (`ok`, `kind`, `us_per_frame` over 30 frames at 960x540, `blank`). Exit code 0 only if it drew. | a second or two |
| `livewall.exe -decode <share> <kick> <parent>` | **Video helper** (`decoder.c`), started by whichever process shows a video (engine, screen saver, `-snapshot`, `-check`), never by hand. It runs sandboxed (`sandbox.c`): an AppContainer with no capabilities, low integrity, in a job (no child processes, 1.5 GB memory, ends with its parent). | while that video is open and not parked |
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
`livewall.status` returns a packed `LRESULT`: status in bits 0–7, fps in 8–23, width in 24–39, height in 40–55, and the speed guard level in 56–57 (0–3: how far a heavy shader has been lowered).

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
| `sandbox.c/.h` | Starts a copy of livewall.exe in the `livewall.decoder` AppContainer (registered once with `CreateAppContainerProfile`; this makes `%LOCALAPPDATA%\Packages\livewall.decoder`), with only the handles it is given, extension points, non-system fonts and remote images blocked, and in a job |
| `decoder.c/.h` | The video helper. Shares one block of memory (`DecodeShare`) with the process that started it, plus an event to wake it. It reads the file through an `IStream` over the handle it was given, decodes with the Media Foundation media engine on the same graphics card (by LUID), and draws each frame, fitted, into a texture shared with an NT handle and a keyed mutex (key 1 = new frame, key 0 = taken). Neither side waits for the other. It writes no log; errors go into `DecodeShare` and the engine logs them. |
| `src_shader.c` | Built-in shaders (bytecode embedded at build time); custom `.hlsl` files compiled at load with `d3dcompiler_47.dll`, which is freed right after. `shader_problem()` turns a compiler error into a short sentence for the settings footer |
| `src_video.c` | The engine side of video: opens the file, starts the helper (`decoder.c`) with that one handle, makes the shared texture at the back buffer size and copies each new frame into the back buffer on the card. Parking ends the helper; unparking starts a new one at the same position. Media Foundation is never loaded into the engine. |
| `src_image.c` | WIC still images (one immutable texture) and animated GIFs (CPU compositing into a dynamic texture) |
| `still.c/.h` | Hands still pictures to the Windows wallpaper (zero-cost mode) |
| `preview.c/.h` | Offscreen rendering through the same sources: settings thumbnails, `-snapshot`, `-check` and the lock screen picture. Videos jump to the requested time first (or a third of the way into a shorter video), so a fade-in from black still gives a picture |
| `engine.c/.h` | Message loop, tray menu, scheduling, pause logic, recovery |
| `lock.c/.h` | Turning livewall's screen saver on and off in Windows, keeping the `.scr` copy up to date |
| `saver.c` | The screen saver window and its draw loop (same sources as the engine) |
| `settings.cpp` | Custom-drawn settings window |
| `gallery.c/.h` | Community tab downloads (settings process only, on worker threads): `index.json`, previews and wallpapers over HTTPS from `manndevani.github.io/livewall-gallery/` only, with size limits (list 1 MB, preview 8 MB, file 60 MB). Each file goes to a `.part` file, is checked against the SHA-256 in the list, then moved into place as `%APPDATA%\livewall\gallery\<id>\<name><ext>` (previews in `gallery\previews\`); file names come from livewall, never from the download. Only wallpapers in the maintainer's signed list (`signed.txt`: id, file SHA-256 and preview SHA-256 per line, ECDSA P-256 signature over the SHA-256 of the rest) are listed; the public keys are `gallery_keys` in `gallery.c`. The last accepted list is kept as `gallery\signed.txt`, and a list with a lower `seq` is refused. WinHTTP and BCrypt are loaded from System32 on first use, so the engine never loads them. Installed wallpapers are added to the library like any other file |
| `shaders/` | HLSL. `fullscreen.hlsl` (VS), `blit` (textures + fit), `dim` (brightness), one file per built-in |

## Engine loop

`MsgWaitForMultipleObjectsEx` waits on a high-resolution waitable timer, and the timer is armed only while something animates. A still image draws once, and then the engine just sleeps.

- **Frame tick** (`fps` from the config; 60 for video, which presents only when a new frame arrives, and no more often than the `fps` cap, through `source_fps`) → `render()`.
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
- **No GPU driver** (some virtual machines, a missing or broken driver) → `gfx_init` falls back to Direct3D's CPU renderer (WARP). `gfx.software` is set (also for the "Microsoft Basic Render Driver"), and Auto resolution then aims at 640×360 instead of 2560×1600. Build with `-DLW_FORCE_WARP=1` to test it on a normal PC. Other test builds: `-DLW_TEST_NO_GALLERY=1` (the gallery never answers, as when offline), `-DLW_TEST_LOCAL_GALLERY=1` (reads the gallery from `http://127.0.0.1:8765/`, a test copy of the site), `-DLW_TEST_LIGHT=1` (light theme in Settings), `-DLW_TEST_DROP=1` (Settings fakes a drop of the files in the `LW_DROP` variable, separated by `|`), `-DLW_TEST_LOST=1` (the engine pretends the graphics card was reset every 90 frames) and `-DLW_TEST_KEY=<128 hex>` (also trust this public key for the gallery list; for a throwaway key from `sign.ps1 -NewKey -KeyName <test name> -NoPassword`). From a shell, `set CL=/DLW_TEST_LIGHT=1` before `build.bat`.
- The process runs at BELOW_NORMAL priority with EcoQoS (power throttling) on, from the moment the wallpaper is first on the desktop. Before that (at sign-in) it runs at normal priority and checks for the desktop every 100 ms instead of 500 ms, for at most 60 s, so a busy sign-in doesn't hold it back. The log line "on the desktop N ms after start" shows where the start-up time went.

## Rendering

- One swap chain covers the whole virtual desktop: FLIP_DISCARD with `DXGI_SCALING_STRETCH`.
- Shader resolution is chosen from the display by default (`resolution=0`, Auto): 1:1 up to 2560×1600 pixels, and 1/2 or less on bigger desktops (4K, wide multi-monitor), where DWM upscales for free. Full (`1`) and Half (`2`) can be picked in settings.
- Video and images ignore the shader resolution setting. A video that the fit mode would stretch up (smaller than the screen) asks for a back buffer at its own size with `gfx_content_size`, and DWM stretches it. `TransferVideoFrame` then only converts colours, and every frame is presented at the smaller size. `v_close` forgets the size, and the next `gfx_resize` goes back to the window size.
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
- **Lock shortcut, Ctrl+Alt+L** (`[lockscreen] shortcut=1`, default on): the engine holds it with `RegisterHotKey` (no keyboard hook, no polling). `lock_now()` sends `WM_SYSCOMMAND / SC_SCREENSAVE` to the desktop window, so Windows starts its configured screen saver exactly as after the idle time and stays in charge of locking when it ends. Every 100 ms `lock_check()` looks for the running `livewall.scr` process (Toolhelp snapshot; the saver window can't be found from the engine's desktop when Windows starts it); if it doesn't show up within 5 s, it calls `LockWorkStation()`. **Windows does the locking itself:** a password-protected screen saver that Windows starts runs on Windows' own locked screen-saver desktop, and when it ends Windows goes straight to the lock screen, with no desktop in between. The only gap is Windows' grace period (`ScreenSaverGracePeriod` in HKLM `...\Winlogon`, 5 s when unset): a screen saver that ends sooner returns to the desktop with no password. So the shortcut sets a named event (`Local\livewall_lock_shortcut`); the saver reads and clears it when it starts, and if input comes during the grace period (plus 0.5 s), it keeps playing until the grace period is over and only then closes. As a backup, a thread-pool wait (`RegisterWaitForSingleObject`) on the saver process checks 2 s after it ends whether Windows has locked (by comparing the count of `WTS_SESSION_LOCK` events the engine has seen, so a quick Windows Hello unlock is never undone) and calls `LockWorkStation()` if not. Tried and dropped: the saver calling `LockWorkStation()` itself on the click. Windows then takes 1–2 s and shows the desktop on the way, whether the saver closes at once, stays frozen, or fades to black. Started any other way (idle, Preview), the saver closes at once on input, as Windows expects. If livewall is not the screen saver, or *Ask for password* is off, the shortcut calls `LockWorkStation()` at once. So the shortcut always ends in a locked PC. The saver ignores key repeats and keys in its first 0.6 s, so the held Ctrl and Alt don't close it.
- **Ask for PIN right away** (only shown with the screen saver and *Ask for password* on): sets `ScreenSaverGracePeriod` to 0, so Windows asks for the PIN on any input. It is a machine-wide value, so the settings window runs `reg.exe` with the `runas` verb (UAC); livewall itself never runs elevated. Before the first change it stores Windows' value in `[lockscreen] grace_before=` (`-` = not set). That value is put back when the switch is turned off, when the screen saver or its password is turned off, and on **Exit** from the tray: the engine re-reads config.ini and starts `livewall.exe -put-back-grace`, a separate process that asks for admin rights (the engine doesn't load the shell). `-quit` (updates, rebuilds) leaves it alone. If the admin prompt is cancelled, `grace_before` stays, so it can still be put back later. While the grace period is not 0, the Lock screen tab shows a warning in the accent color.
- Windows+L can't be taken over: it is handled by Windows itself, and the only way around that (turning off Windows' own lock and catching the keys) would weaken the PC's lock, so it is not done.

## Linux (`linux/`, since 2026-11-20)

A separate, small C program (~80 KB) with the same ideas as the Windows engine. It doesn't share code with `src/`, only the config format and the shaders.

| File | Responsibility |
|---|---|
| `main.c` | Commands (`-set`, `-list`, `-quit`, `-pause`, `-snapshot`, `-lock`…), the engine loop (`poll()` on the display, a timerfd for frames, a signalfd, libmpv's wake-up eventfd), EGL setup, battery check, XDG autostart entry |
| `config.c` | `~/.config/livewall/config.ini` (same sections and keys as Windows; `autostart` instead of `start_with_windows`). Written atomically (temp file + rename) |
| `gl.c` | OpenGL ES 2.0: builds shaders as `prelude.glsl` + body + `epilogue.glsl`; Shadertoy-style custom files get a small adapter. Lower resolution renders into a texture that is then stretched. Brightness is a black quad with alpha |
| `media.c` | Videos, GIFs, pictures through libmpv's render API, loaded with `dlopen` (optional). `hwdec=auto-safe`, `profile=fast`, no audio, small demuxer cache |
| `wayland.c` | One wlr-layer-shell surface per monitor on the *background* layer. Draws only after the compositor's frame callback, so it stops by itself when hidden |
| `x11.c` | One `_NET_WM_WINDOW_TYPE_DESKTOP` window covering the screen, kept below. "Covered" = the active window is fullscreen or maximized |
| `png.c` | Tiny PNG writer for `-snapshot` |

- The shaders live in `shaders/glsl/` and are GLSL ES 1.00 twins of `src/shaders/*.hlsl`. `pos` is in pixels with y down, like Direct3D, so the two versions match line for line. Android and macOS will use the same files.
- Commands talk to the running engine with signals: they write `config.ini` and send `SIGHUP` (reload), `SIGUSR1` (pause), `SIGUSR2` (resume) or `SIGTERM` (quit). The pid file is `$XDG_RUNTIME_DIR/livewall.pid`, held with `flock`.
- **Lock screen** (since 2026-11-22). livewall never checks a password. `-lock` runs `swaylock-plugin --command '<livewall> -lockbg'` when that locker is installed: swaylock-plugin locks the session with the Wayland lock protocol and runs `-lockbg` inside its own small Wayland server as the background. `-lockbg` is the normal engine with the `[lockscreen] wallpaper` (same key as Windows; empty = the desktop's), no pid file and no autostart, Wayland only, and it exits when the locker closes its server on unlock. Without swaylock-plugin, `-lock` runs `loginctl lock-session` (the desktop's own locker). `-lock-picture` renders a still of the lock wallpaper for lockers that take a picture.
- When the Wayland connection closes (the desktop or the locker went away), `dispatch` returns false and the engine exits instead of polling a dead connection.
- `make -C linux` needs the dev packages listed in `docs/LINUX.md`. The layer-shell protocol XML is kept in `linux/protocols/` (from the wlroots project, permissive license).
