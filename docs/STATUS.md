# Status

Last updated: 2026-11-22

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
- [x] Community tab in Settings (2026-11-22, `gallery.c`): lists the gallery from GitHub Pages with previews; one click downloads, checks the SHA-256 and applies. Verified with Settings using the real `%APPDATA%`: listing; install to the desktop (file saved under `gallery\soft-waves\`, the engine showed it, green dot); install from the Lock screen tab (sets only the lock wallpaper); "Couldn't reach the gallery" plus Try again (test build with `-DLW_TEST_NO_GALLERY=1`, both attempts logged). A thumbnail that failed before its file was installed is rebuilt after the install. Not in a release yet (after 0.2.1).
- [x] Gallery web page and likes (2026-11-22, in the gallery repo: `web/`, `tools/likes.ps1`): a page that lists the gallery from the same index.json (search, type filter, most liked or by name, a details window with download and "View the code", a link per wallpaper as `#w/<id>`). It loads nothing from other sites (strict Content-Security-Policy), and shows list text as plain text only: a hostile entry in a test list ran no code and its outside links were dropped. Checked at desktop and phone width, dark and light, from a local server. Likes are upvotes on one Discussions post per wallpaper; the publish job makes the posts and writes `likes` into index.json every 6 hours. livewall's Community tab sorts by likes and shows "♥ N" (checked with a test build reading a local copy). **Not yet seen on GitHub**: the page on Pages, and `likes.ps1` (needs Discussions turned on with a "Wallpapers" category).
- [x] Gallery checks with a video (2026-11-22, run locally: `prepare.sh` in WSL with a stand-in for ClamAV, then `check.ps1` and `build-index.ps1`): ffmpeg's copy drops the sound track, the title and the GPS location; the check passes it as a video; index.json lists it, and the published file is the rebuilt copy.
- [x] Video previews no longer use the first frame (2026-11-22): `-snapshot`, `-check` and the "Same picture on lock screen" picture jump to the requested time, or a third of the way into shorter videos. Before, a video that fades in from black was judged blank by `-check`, so the gallery would have refused it; now a 12 s fade-in is pictured at 4 s and passes.
- [x] Custom shader errors in Settings (2026-11-22): the footer says why the chosen `.hlsl` can't be shown, e.g. "broken.hlsl can't be shown: line 3: undeclared identifier 'wobble'", that it has no mainImage function, or that the file is missing. Settings compiles the file itself (once per file, and again when the window is brought back), so it shows even while the engine is paused. Checked with three broken files.
- [x] Light theme of the settings window (2026-11-22, test build with `-DLW_TEST_LIGHT=1`): the library, Community and Lock screen views were captured and look right.
- [x] Safety review (2026-11-22): threats, what livewall does and what is still open are written up in [SAFETY.md](SAFETY.md). Fixes from it:
  - DLLs and `reg.exe` only from System32: 7 of 8 fake DLLs placed next to the exe were loaded before; after the fix none of 9 are, and videos, pictures and custom shaders still work.
  - Memory handed back after a switch: shader after a video 18.1 MB → 1.1 MB working set.
  - A wallpaper that makes Windows reset the graphics card twice within 2 minutes is replaced by a built-in until livewall restarts. Checked with a test build that fakes the reset (`-DLW_TEST_LOST=1`); a real reset was not caused on purpose.
  - Drag and drop onto Settings adds files to the library (a folder and a `.txt` were skipped, a shader and a video added). Checked with a test build that fakes the drop (`-DLW_TEST_DROP=1`); **not yet dragged by hand** from File Explorer.

## Lock screen on Windows (screen saver, 2026-11-20)

- [x] `livewall.exe /s` draws the lock wallpaper full screen over every monitor, animates (~1% of one core for Aurora), hides the cursor, and closes on a key press or mouse movement. Memory drops from 35 MB to 2.5 MB after 5 s.
- [x] `/p <hwnd>` draws inside a host window and exits when the host closes (tested with a stand-in window, not yet in Windows' own dialog).
- [x] Settings window: new Desktop / Lock screen switch. On the Lock screen tab, clicking a card sets `[lockscreen] wallpaper=` and leaves the desktop alone; *Same as desktop*; *Preview* runs `/s` with the chosen wallpaper (captured and clicked). `/c` opens settings on that tab.
- [ ] **Not yet clicked: *Show when idle*, *start after*, *Ask for password*.** They change Windows' own screen saver and lock settings, so the user turns them on. Then check: the setting shows in Windows' Screen Saver Settings, the screen saver starts after the wait, and Windows asks for the password when it ends.
- [ ] **Not yet clicked: *Same picture on lock screen*** (WinRT `LockScreen.SetImageFileAsync` from an unpackaged app). Turning it off does not bring the old picture back; the user picks one again in Windows Settings.
- [x] Lock shortcut Ctrl+Alt+L: the engine holds the keys while running (another app could not take them; free again with the engine stopped). The user pressed it with the screen saver still off: it locked directly, as designed (log: "screen saver is off, locking directly"). Since then its switch shows only once "Show when idle" is on, and a note says so before that.
- [x] Ctrl+Alt+L with the screen saver on and "Ask for password" on (user, 2026-11-20): the wallpaper showed within 500 ms each time and stayed until clicked. Clicking within 3 s let the user back in without the PIN: Windows' 5 s grace period. Fixed by locking as soon as the shortcut's screen saver closes. The first version of that fix looked for the saver window, never found it, and only locked after the 5 s fallback (user saw the desktop for a few seconds). It now looks for the `livewall.scr` process: confirmed, the log shows the lock about 1 s after the click. To remove that last second, the saver locks the PC itself on the click when the shortcut started it. The user still saw their open windows for a moment, because the saver closed before the lock screen appeared; it now stays up until Windows reports the lock (confirmed: no windows showed). That felt slow, since Windows takes about 1 s to lock. A fade to black did not help, and freezing the frame still showed the desktop: Windows passes through it after `LockWorkStation()`. Now livewall locks nothing itself: the saver keeps playing through the grace period and then ends, and Windows locks natively from its screen-saver desktop. Not yet confirmed by the user.
- [x] "Ask for PIN right away" switch and its warning drawn on the Lock screen tab (capture, 2026-11-20). Not clicked: the UAC prompt and the reg.exe change, the put-back on switch off, and on tray Exit (`-put-back-grace`) are untested.
- [ ] The settings switch for the shortcut was drawn but not clicked against the real engine.
- [ ] Battery (one still frame) and display-off pausing in the screen saver: written, not tested.
- [ ] Multiple monitors in the screen saver: one window over the whole virtual screen, like the engine; untested.

## Linux (`linux/`, 2026-11-20): verified in WSL2 Ubuntu 26.04 without a GPU

- [x] Builds with no warnings (~80 KB). The version comes from `src/version.h`.
- [x] All five built-ins render through `-snapshot` and match the Windows pictures; `examples/plasma.glsl` (Shadertoy-style) works.
- [x] Video, GIF and PNG through libmpv (`-snapshot` and live under Sway).
- [x] Wayland (headless Sway): draws on the background layer, `-set` switches while running, pauses when a fullscreen window covers it and resumes when it closes, `-pause`/`-resume`, `-quit`.
- [x] X11 (Xvfb): draws, the pointer moves Glow Grid, `-quit`.
- [x] Lock screen commands (2026-11-22, headless Sway): `-lock-set` (and `same`), `-lock-picture` (default and given path and size; a silly size is refused), `-lockbg` draws the lock wallpaper (Dunes) while the desktop shows Aurora, `-lock` hands swaylock-plugin a correctly quoted command even for a path with a `'` in it (tested with a stand-in script), and falls back to `loginctl lock-session` without it. When Sway quits, both the engine and the lock background log "the connection to the desktop was closed" and exit (before this fix the engine would have spun on the dead connection).
- [ ] The real swaylock-plugin hasn't been run yet (it isn't packaged for Ubuntu). Not checked: that it accepts `-lockbg` as its background, and unlock closing it.
- [ ] Not yet tried on a real Linux PC with a GPU: cost, multi-monitor, HiDPI scale, KDE Plasma, Hyprland, a real X11 desktop with icons, libmpv hardware decoding, the XDG autostart entry, pause on battery.
- [ ] No settings window or tray icon on Linux yet; everything is a command.
- [ ] GNOME can't be supported this way (no layer-shell).

## Releasing

1. Bump `src/version.h`.
2. Commit and push.
3. `git tag -a vX.Y.Z -m "livewall vX.Y.Z"` and `git push origin vX.Y.Z`.

GitHub Actions (`.github/workflows/build.yml`) builds the Windows exe and the Linux binary (and checks every built-in renders on Linux), then creates the release with both downloads. Every push to `main` also builds, and the zip is kept as a downloadable workflow artifact.

**Code signing:** SignPath Foundation (free for open source) **turned the application down on 2026-11-20**. The reason was not enough public visibility yet: they look for stars, forks, contributors, articles and discussions (Reddit, YouTube…). They invited a new application once the project is better known; a paid SignPath plan is the other route. If approved later:
- add a signing step to the workflow, after Package and before Publish;
- add the line "Free code signing provided by SignPath.io, certificate by SignPath Foundation" to the README's Code signing policy section.

SignPath's conditions: the build must be verifiable from source, each signing needs manual approval, product name and version must be set, GitHub needs MFA, and the README needs a code signing policy, privacy statement and uninstall steps.

- [x] Signed gallery list (2026-11-22, `gallery.c`, and `tools/sign.ps1` in the gallery repo; see SAFETY.md). Tested against a local copy of the gallery with a throwaway key: a good list shows all 3 wallpapers; a changed fingerprint in index.json hides that one; one changed letter in signed.txt, a missing signed.txt, or an older list (lower seq) after a newer one shows "The gallery's list doesn't carry the right signature"; installing still works; a second publish keeps the published files byte for byte; `sign.ps1` refuses to build on a signed.txt changed without the key. **Blocks the next release and the gallery push:** `gallery_keys` is empty until the maintainer runs `tools\sign.ps1 -NewKey` and the public key is added, so the Community tab shows the signature message. Then sign, push the gallery, and check it on Pages.

## Implemented but not yet verified
- [ ] Wallpaper gallery (2026-11-22): separate repo https://github.com/manndevani/livewall-gallery (local copy at `C:\Projects\livewall-gallery`) with sharing rules, `tools/check.ps1` (uses `livewall.exe -check`), `tools/prepare.sh` (scope rule, ClamAV, ffmpeg rebuild), `tools/build-index.ps1` and the `gallery.yml` workflow (checks on pull requests, publishes to GitHub Pages from main). Checked on this PC: two example shaders pass; `#include`, a flat colour, a 28x-heavy shader, a stray .exe, bad info.json and bad folder names are refused; a fork PR touching README or using a folder name with `$(...)` is refused without running it. On GitHub (first push to main): the ClamAV scan, the check with livewall built from main (on WARP, no GPU) and the Pages publish all passed; https://manndevani.github.io/livewall-gallery/index.json lists both wallpapers and every file matches its SHA-256. Not yet seen on GitHub: a pull request, and a media file going through ffmpeg. Next: a gallery website, and likes through GitHub Discussions.
- [x] Faster start at sign-in (2026-11-22): full speed and 100 ms checks until the wallpaper is on the desktop; the startup task is no longer re-registered at every sign-in (a bug), and housekeeping waits until the wallpaper shows. Warm start: 250–300 ms. Cold start (power-on): 4.7 s before the bug fix, 3.0 s after it (user, 2026-11-22). Most of the rest is the first Direct3D device.

- [x] **Start after a reboot** (2026-11-20): the sign-in task started the engine 19 s after boot, and the wallpaper attached. That first boot opened the settings window, because the real config did not exist yet (see FINDINGS.md). Fixed with `-startup`: tested with no config at all through `schtasks /Run`, and no window appeared. A second sign-in start exits silently, and opening the exe by hand still shows settings.
- [ ] Interactive shaders reacting to the real mouse and clicks on the desktop. The shaders render with a simulated cursor, but live input has not been checked by eye.
- [ ] Tray menu: switching wallpapers, Add, Pause, Start with Windows, Exit. The code path is shared with the settings window, but the menu itself has not been clicked.
- [ ] The hover ✕ that removes a library item. Can't be shown with synthetic mouse messages, because `TrackMouseEvent` sends `WM_MOUSELEAVE` at once.
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
- The tray menu has still not been clicked by hand (2026-11-22): a right-click sent from a script didn't show the menu, probably because Windows only lets the program the user last clicked bring a menu to the front.

## Ideas / next

- From the safety review (details in [SAFETY.md](SAFETY.md)): decode untrusted files in a low-rights helper process; slow a shader down when it takes too long on the graphics card; share wallpapers through an issue form instead of a pull request; accept `.glsl` on Windows; show names without `.hlsl`; measure Linux memory on a real graphics card.

- Other platforms: Android and macOS next. They will reuse `shaders/glsl/`.
- Lock screen on the other platforms, always through the system's own lock (never a home-made password screen):
  - Linux: done through swaylock-plugin or the desktop's own locker (see the Linux section above);
  - Android: a live wallpaper shows on the lock screen by itself;
  - macOS: a `.saver` plus "Require password";
  - iOS: export a video or Live Photo only.

- Per-monitor wallpapers (one swap chain per monitor).
- Reduce video cost: decode at the source size and let DWM scale, as the shaders do.
- An installer or "copy to %LOCALAPPDATA%" option so the task doesn't point into a build folder.
