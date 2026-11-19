# livewall

A tiny live wallpaper for Windows 10/11: shaders, videos, GIFs, images and your own HLSL shaders, drawn behind the desktop icons.

- **Light:** a ~90 KB exe with no runtime and no installer. About 14 MB of memory and ~0.1% CPU for a shader wallpaper. It stops drawing completely whenever nobody can see it.
- **Starts with Windows**, early: a logon task starts it before the usual startup apps.
- **Settings window** that follows your light/dark theme and accent color. It runs as a separate process, so the wallpaper itself never carries UI memory.

## Build

Requires Visual Studio 2022 (or the Build Tools) with **Desktop development with C++**.

```
build.bat
```

Output: `build\livewall.exe`.

Or skip building: download `livewall.exe` from the [Releases](https://github.com/manndevani/LiveWallpaper/releases) page and run it. Windows may show a "Windows protected your PC" warning because the file isn't code-signed. Click **More info → Run anyway**.

## Use

Run `build\livewall.exe`. The first time, it opens the settings window and registers itself to start at sign-in.

- **Tray icon:** click for settings; right-click to switch wallpaper, add files, pause, toggle *Start with Windows* or exit.
- **Settings:** click a card to apply it. Use **Add wallpaper** for videos (`mp4 mov mkv webm wmv avi`), GIFs, images (`png jpg bmp webp tif jxr`) or shaders (`.hlsl`).
  - Adjust speed, brightness, frame cap, shader quality or fit (cover/contain/stretch).
  - Choose when to pause.

| Command | Effect |
|---|---|
| `livewall.exe` | Start the wallpaper (or open settings if it's already running) |
| `livewall.exe -settings` | Open the settings window |
| `livewall.exe -add` | Open settings and the file picker |
| `livewall.exe -quit` | Stop the wallpaper |
| `livewall.exe -snapshot <id or file> <out.png> [seconds]` | Render one frame to a PNG |

Settings are stored in `%APPDATA%\livewall\config.ini`. Turning off *Start with Windows* removes the scheduled task.

## It pauses when

- windows cover every pixel of every monitor (maximized and fullscreen apps count);
- the session is locked or the display is off;
- the PC is on battery (optional);
- you pause it.

When paused, the last frame stays on screen, at zero GPU cost. After a couple of seconds paused, livewall gives its memory back to Windows: about **0.3 MB** for a paused shader and **1.2 MB** for a paused video.

**Still pictures cost nothing:** livewall hands them to Windows as the normal desktop wallpaper and keeps only its tray icon (about **0.5 MB**).

## Interactive and custom wallpapers

*Ripples* and *Glow Grid* react to your cursor and clicks on the desktop. Write your own in HLSL: see [docs/CUSTOM_SHADERS.md](docs/CUSTOM_SHADERS.md).

## Roadmap: other platforms

The goal is the lightest live wallpaper on every platform that allows one.

| Platform | Possible? | How |
|---|---|---|
| Windows 10/11 | ✅ done | Direct3D 11 behind the desktop icons |
| Linux | ✅ planned | Wayland `wlr-layer-shell` background layer (KDE, Sway, Hyprland…), X11 desktop window; OpenGL ES. GNOME needs an extension. |
| macOS | ✅ planned | Borderless window at desktop level, under the icons; Metal |
| Android | ✅ planned | The official `WallpaperService` live-wallpaper API; OpenGL ES |
| iPhone / iPad | ⚠️ limited | iOS doesn't let apps run live wallpapers. The best possible is exporting a wallpaper as a video or Live Photo that you set yourself. |

To share work between platforms, the shaders and the settings format will move into a common core first.

## More

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it is built
- [docs/FINDINGS.md](docs/FINDINGS.md): how it gets behind the icons, measurements, Windows quirks
- [docs/STATUS.md](docs/STATUS.md): what works, what is untested, known limitations

## Contributing

Bug reports, shaders, testing on other hardware and ports are all welcome. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT: free for everyone to use, change and share. See [LICENSE](LICENSE).
