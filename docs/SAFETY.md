# How livewall keeps your PC safe and light

A live wallpaper runs all day, opens files that other people made, and sits next to the lock screen. This page lists the ways that could hurt you, what livewall does about each one, and what is still open. It was written after a review on 2026-11-22; the "Still open" parts are the to-do list.

## 1. Safety

### Wallpapers that carry a harmful program

**The risk.** In 2026, security researchers found wallpapers in another app's community store that came with programs and password-protected archives inside them. People who opened them got programs that stole passwords or locked their files ([Kaspersky, BleepingComputer](#sources)).

**What livewall does.**
- A livewall wallpaper is only a picture, a GIF, a video or a shader (a short text file of drawing code). livewall never runs a program that comes with a wallpaper, and the gallery doesn't accept programs, scripts or zip files at all.
- Every gallery file is checked by a computer and then by a person before it is listed: it is opened and drawn by livewall itself, scanned with ClamAV, and videos, GIFs and pictures are rebuilt with ffmpeg so that only the picture data is kept.
- livewall checks every download against a SHA-256 fingerprint from the gallery list and throws away a file that doesn't match. Downloads go over HTTPS only, and are saved under a name livewall makes itself (lowercase letters, numbers and dashes), so a list entry can't write anywhere else on your PC.

- **The list is signed (2026-11-22).** The maintainer signs a short list of each approved wallpaper and the fingerprints of its file and preview (`signed.txt`), with a key kept in Windows' key store on the maintainer's PC. The key can't be copied out, and Windows asks for its password each time it signs. livewall has the public half built in, checks the signature with Windows' own BCrypt, and shows only wallpapers that are on the signed list with the same fingerprints. So even someone who took over the GitHub account couldn't add or change a wallpaper that livewall shows. livewall also remembers the last list it accepted and refuses an older one, so a wallpaper taken off the list can't be brought back by serving an old list.

**Still open.** The GitHub account should still have two-step sign-in turned on: someone who took it over could take wallpapers off the gallery, though not add their own. If the signing key is lost, a new livewall release with a new key is needed.

### Files made to break the decoder

**The risk.** A video or picture can be made on purpose to trigger a bug in the code that decodes it. Windows' own media code has had such bugs fixed many times in 2025 and 2026 (for example CVE-2025-26674, CVE-2025-53131, CVE-2026-57094), and the WebP picture library had one in 2023 that was used in real attacks (CVE-2023-4863).

**What livewall does.**
- It uses the decoders built into Windows (WIC for pictures, Media Foundation for videos), so Windows Update fixes them; livewall ships no decoder of its own. On Linux, videos go through the system's libmpv, which the system's package updates fix.
- Gallery media is rebuilt by ffmpeg in the gallery's checks, so a crafted file sent to the gallery is not what people download.
- Pictures larger than 8192 pixels on a side are scaled down, and a gallery file larger than 60 MB is refused.

**Still open.** Your own files, and the decoding itself, are handled inside the wallpaper process with your normal user rights. Fix: decode untrusted files in a separate helper process with lowered rights (a "low integrity" process), so a decoder bug can't reach your files.

### Shaders

**The risk.** A shader runs on the graphics card. It can't open files or use the internet, but a shader that is far too heavy can freeze the screen, and Windows then resets the graphics card. A browser has the same problem with web pages that draw with the graphics card.

**What livewall does.**
- Shaders get only the time, the screen size and the mouse position. They can't open files, use the network or talk to other programs, and livewall refuses `#include`, the one way a shader could ask for a file. Shader files over 1 MB are refused.
- The gallery measures how much work each shader takes and turns down one that takes more than 15 times the work of a simple reference shader.
- **New (2026-11-22):** if the graphics card is reset twice within 2 minutes while the same wallpaper is showing, livewall shows a built-in wallpaper instead and doesn't load that one again until it restarts. Before, it loaded the same shader again after each reset, which could freeze the screen over and over. Windows may block a program from the graphics card after many resets.

**Still open.** A shader that is heavy but not heavy enough for a reset makes the graphics card work hard and drains the battery. Fix: measure the time each frame takes on the graphics card and lower the frame rate or resolution, or warn in Settings, when a shader is too slow.

### A fake DLL in the same folder

**The risk.** When a program starts, Windows looks for some of the system files it needs (DLLs) in the program's own folder first. If livewall.exe is in Downloads and a harmful file with the right name is there too, it would be loaded.

**What livewall does (fixed 2026-11-22).** Tested with fake files under eight system DLL names: seven were loaded. Now the exe tells Windows to load its DLLs from System32 only (`/DEPENDENTLOADFLAG` plus `SetDefaultDllDirectories`), and the PIN switch starts `reg.exe` by its full System32 path. After the fix all nine fake files were ignored.

### The lock screen

livewall never asks for, sees or checks your password. On Windows the lock wallpaper is a screen saver, and Windows itself locks the PC and shows its normal sign-in screen when it ends. On Linux livewall only draws the background: `swaylock-plugin` (or the desktop's own locker) checks the password. The one step that needs administrator rights (allowing the PIN to unlock after the screen saver) asks Windows first with the normal UAC prompt.

### Other programs talking to livewall

The Settings window and the wallpaper process talk only through three window messages: reload the settings, pause, and ask for the status. The settings live in a file in your own folder. A program that can send these messages or edit that file already runs as you, so it gains nothing new through livewall.

### The gallery web page

The page loads nothing from other sites (a strict Content-Security-Policy), shows names and descriptions as plain text only, and only links to files inside the gallery and to its own discussion posts. A test list with harmful entries ran no code on the page.

### The download itself

Releases are built from the public source code by GitHub Actions. The exe is not signed yet, so Windows SmartScreen warns about it; signing is planned once the project qualifies for a free certificate.

## 2. Memory and battery

livewall should cost almost nothing while you work.

| Measured on 2026-11-22 (Windows 11, hybrid laptop) | RAM in use (working set) |
|---|---|
| Just started, showing a shader | 0.9 MB |
| Playing a video | 3.4 MB |
| Back on a shader after a video, before the fix | 18.1 MB |
| Back on a shader after a video, after the fix | 1.1 MB |

- **"Private bytes" looks large, but isn't RAM.** Task Manager can show about 100 MB of "commit" for livewall. Almost all of it is memory the graphics driver reserves for the window; it is not in use and costs nothing until needed. The working set above is what livewall really uses.
- **Fixed (2026-11-22):** after switching from a video to another wallpaper, the video decoder's memory (about 15 MB) was kept until livewall restarted. Now a switch hands it back within a few seconds.
- **No leaks found:** 210 switches between all wallpaper types, broken files included; memory, handles and threads stayed level.
- **Stops when no one can see it:** it pauses when the desktop is covered, on battery, when the PC is locked and when the display is off. During a long pause it frees the video decoder. It runs at below-normal priority in Windows' efficiency mode, and nothing is allocated per frame.

**Still open.** Memory on Linux has only been measured in a test setup without a real graphics card.

## 3. Adding your own wallpapers

| What | Now |
|---|---|
| Add a file | "Add wallpaper" in Settings, or **drag files onto the Settings window** (new, 2026-11-22). Folders and other file types are skipped. |
| Get one from the gallery | Community tab, one click to install, most liked first. |
| A shader with a mistake | Settings says what is wrong and on which line, e.g. "line 12: undeclared identifier 'tme'". |
| Write a shader | [CUSTOM_SHADERS.md](CUSTOM_SHADERS.md): one `mainImage` function, same inputs as Shadertoy. |

**Still open.**
- **Sharing needs a GitHub pull request**, which most people have never made. Fix: a GitHub issue form where you attach the file and fill in the name and licence, and a gallery job that turns it into a pull request for review.
- **Two shader languages:** Windows uses HLSL and Linux uses GLSL (the language of Shadertoy), so a Shadertoy shader has to be translated by hand for Windows. Fix: accept `.glsl` on Windows too, translating it with a small converter for the common Shadertoy functions.
- **Shader names show `.hlsl`** in the library (e.g. "sw.hlsl"). Fix: show the file name without the extension.

## Sources

- Kaspersky, press release on wallpapers carrying harmful programs (June 2026); also reported by BleepingComputer ("Steam Workshop abused to spread malware via Wallpaper Engine app") and Windows Central.
- Rapid7, CVE-2025-26674 (Windows Media remote code execution); Action1, CVE-2025-53131 (Windows Media); STAR Labs, root-cause analysis of CVE-2026-57094 (HEVC decoder heap overflow).
- Red Hat, CVE-2023-4863 (libwebp heap overflow); NCSC Ireland advisory on the WebP flaw.
- Chromium, "WebGPU Technical Report" (how a browser checks shaders before running them); LWN, "WebGL security" (lwn.net/Articles/444672); arXiv 2410.11075 (graphics card attacks from shaders).
- Microsoft Learn: "Dynamic-link library security", `SetDefaultDllDirectories`, `/DEPENDENTLOADFLAG`; "Timeout detection and recovery (TDR)".
