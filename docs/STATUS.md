# Status

Last updated: 2026-11-20

## Done and verified on the user's PC (Windows 11 25H2, hybrid Intel + NVIDIA)

- [x] Wallpaper renders behind the desktop icons (24H2+ layout). The user confirmed it visually in v1.
- [x] Five built-in shaders: Aurora, Dunes, Night Sky, Ripples (interactive), Glow Grid (interactive). Each checked with `-snapshot`.
- [x] Custom HLSL shaders (Shadertoy-style `mainImage`): `examples/plasma.hlsl` compiles and renders.
- [x] Still images (PNG), animated GIF and MP4 video: each renders correctly through `-snapshot`, using generated test files.
- [x] Settings window:
  - library grid with real thumbnails for every type, and filters;
  - inspector: status, Pause/Stop, speed, brightness, toggles, FPS cap, quality/fit;
  - footer telemetry.
  - Follows dark mode and the Windows accent color.
- [x] Clicking a card writes `config.ini` and the engine switches (`livewall.reload`).
- [x] Start at sign-in: Task Scheduler logon task `livewall` registered on first run (`schtasks /Query /TN livewall` → Ready).
- [x] Engine cost measured (see FINDINGS.md): ~3 MB / ~0.1% CPU for shaders while drawing (14 MB before the 2026-11-20 trim work); 0 GPU when covered.
- [x] Memory while drawing (2026-11-20): one trim 5 s after drawing starts takes shaders from 14.5 to ~3 MB, steady over 3 minutes, ~1 page fault/s. Video in real-time mode is 14 MB instead of 36 at the same 29–30 fps. `tools/wsdetail.ps1` shows where memory goes.
- [x] Software rendering fallback (2026-11-20, forced with `-DLW_FORCE_WARP=1`). Shaders, GIF and MP4 all run on it. Snapshots of the shader and the video are correct, and both hold 29 fps live. A shader at Auto (640×360) costs ~2.6% of 24 threads (~60% of one core) and 0.7 MB; video costs ~0.6% CPU.
- [x] Settings window memory (2026-11-20): ~19 MB while building thumbnails, then ~3–4 MB while open (was ~18 MB the whole time). The window still draws and clicks correctly after the trim (captured and clicked).
- [x] `-quit` waits for the engine to exit, so stop-then-start works in scripts.
- [x] Idle memory (2026-11-20):
  - paused shader **0.3 MB**;
  - paused video **1.2 MB**: the decoder is freed after 10 s and playback resumes at the same position (verified);
  - still picture **0.5 MB**: handed to Windows' own wallpaper, with brightness applied through a dimmed copy (verified).
- [x] If Windows refuses a picture, livewall falls back to drawing it itself (seen in testing).

- [x] Public release v0.1.0 on GitHub (2026-11-20), with the zip attached; a fresh download matched the local build.
- [x] Shader resolution **Auto** (default), Full or Half (2026-11-20):
  - Auto renders 1:1 on this 1920×1080 screen, measured at ~100 ms/s GPU vs ~27 at half.
  - The settings switch saves `resolution=` and removes the old `scale=` key; both directions were clicked and checked.
  - Released in v0.1.1.
- [x] Animated preview in the README (`docs/images/preview.gif`, 2026-11-20): five built-ins at 2× speed, made from `-snapshot` frames. An MP4 version for posts is made the same way (not in the repo).
- [x] `livewall.exe` carries version info (product name `livewall`, version from `src/version.h`).

## Releasing

1. Bump `src/version.h`.
2. Commit and push.
3. `git tag -a vX.Y.Z -m "livewall vX.Y.Z"` and `git push origin vX.Y.Z`.

GitHub Actions (`.github/workflows/build.yml`) builds the exe and creates the release with the zip. Every push to `main` also builds, and the zip is kept as a downloadable workflow artifact.

**Code signing:** SignPath Foundation (free for open source) **turned the application down on 2026-11-20**. The reason was not enough public visibility yet: they look for stars, forks, contributors, articles and discussions (Reddit, YouTube…). They invited a new application once the project is better known; a paid SignPath plan is the other route. If approved later:
- add a signing step to the workflow, after Package and before Publish;
- add the line "Free code signing provided by SignPath.io, certificate by SignPath Foundation" to the README's Code signing policy section.

SignPath's conditions: the build must be verifiable from source, each signing needs manual approval, product name and version must be set, GitHub needs MFA, and the README needs a code signing policy, privacy statement and uninstall steps.

## Implemented but not yet verified

- [x] **Start after a reboot** (2026-11-20): the sign-in task started the engine 19 s after boot, and the wallpaper attached. That first boot opened the settings window, because the real config did not exist yet (see FINDINGS.md). Fixed with `-startup`: tested with no config at all through `schtasks /Run`, and no window appeared. A second sign-in start exits silently, and opening the exe by hand still shows settings.
- [ ] Interactive shaders reacting to the real mouse and clicks on the desktop. The shaders render with a simulated cursor, but live input has not been checked by eye.
- [ ] Tray menu: switching wallpapers, Add, Pause, Start with Windows, Exit. The code path is shared with the settings window, but the menu itself has not been clicked.
- [ ] The hover ✕ that removes a library item. Can't be shown with synthetic mouse messages, because `TrackMouseEvent` sends `WM_MOUSELEAVE` at once.
- [ ] Light theme of the settings window. Only dark mode was captured.
- [ ] Per-monitor DPI change (moving the settings window to a monitor with different scaling).
- [ ] Explorer restart while running: worked in v1, not re-tested after the refactor.
- [ ] Pause on battery, display off and session lock: worked in v1, not re-tested.
- [ ] "Use integrated GPU" switching at runtime.
- [ ] Windows 10 and pre-24H2 Windows 11 (legacy WorkerW layout). Written but untested; no such machine is available.
- [ ] AMD GPUs. There is no vendor-specific code (standard D3D11 at feature level 10_0+, DXVA through Media Foundation), but it has only run on Intel and NVIDIA so far.
- [ ] Windows on ARM (Snapdragon). The release is x64 only, so it would run under emulation; untested.

## Known limitations

- One swap chain spans all monitors. On mixed-DPI or mixed-refresh setups that works, but it isn't per-monitor. Different wallpapers per monitor are not supported.
- Video `Stretch` is applied by the video processor's destination rect. Some codecs may still letterbox.
- Video is the most expensive type (~14 MB and ~140 ms/s GPU for a small 640×360 clip). Larger 4K files will cost more.
- No audio, by design. No web/HTML wallpapers.
- Choosing a still picture **changes the Windows wallpaper setting**: that is how it costs nothing. The previous Windows wallpaper is not restored when you switch away. It is hidden behind the live one, and shows again only if livewall exits.
- Settings for a still picture (fit and brightness) are applied by Windows. The cover/contain/stretch modes map to Fill/Fit/Stretch.
- Custom shader errors only go to `livewall.log`. The UI shows the "could not show" fallback but not the compiler message.

## Ideas / next

- Other platforms (Linux, macOS, Android). This needs a shared portable core first; see the plan in the README's roadmap.

- Show shader compile errors in the settings inspector.
- Per-monitor wallpapers (one swap chain per monitor).
- Reduce video cost: decode at the source size and let DWM scale, as the shaders do.
- An installer or "copy to %LOCALAPPDATA%" option so the task doesn't point into a build folder.
