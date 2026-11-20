# livewall on Linux

The same five animated wallpapers as on Windows, plus your own videos, GIFs, pictures and shaders, drawn behind your windows. Like on Windows, it stops drawing when nobody can see it.

## Which desktops work

| Desktop | Works? | Notes |
|---|---|---|
| Sway, Hyprland, river, Wayfire, niri, labwc and other wlroots-style desktops | ✅ | Pauses by itself when a fullscreen window covers it or you switch to another workspace. |
| KDE Plasma 6 (Wayland) | ✅ | |
| X11 desktops (XFCE, MATE, Cinnamon, KDE on X11, i3, Openbox…) | ✅ | Pauses when the active window is fullscreen or maximized. If your desktop shows icons, livewall may cover them. |
| GNOME | ❌ | GNOME doesn't let apps draw behind the windows. |

## Install

1. Download `livewall-…-linux-x64.tar.gz` from [Releases](https://github.com/manndevani/LiveWallpaper/releases/latest) and unpack it.
2. For videos, GIFs and pictures, install **libmpv** (shaders work without it):
   - Ubuntu, Debian, Mint: `sudo apt install libmpv2`
   - Fedora: `sudo dnf install mpv-libs`
   - Arch: `sudo pacman -S mpv`
3. Run `./livewall`. It starts drawing and adds itself to your desktop's startup programs.

On Sway or Hyprland, the startup entry isn't read; add a line to your config instead:

```
exec /path/to/livewall -startup          # Sway
exec-once = /path/to/livewall -startup   # Hyprland
```

## Use it

| Command | What it does |
|---|---|
| `livewall` | Start the wallpaper |
| `livewall -list` | Show the built-in wallpapers and your own files |
| `livewall -set dunes` | Switch to a built-in wallpaper |
| `livewall -set ~/Videos/rain.mp4` | Use your own video, GIF, picture or `.glsl` shader |
| `livewall -speed 150` | Animation speed in percent (10–400) |
| `livewall -brightness 70` | Darken the wallpaper (10–100) |
| `livewall -fps 30` | Frame rate cap |
| `livewall -fit cover` | `cover`, `contain` or `stretch` for videos and pictures |
| `livewall -pause`, `-resume` | Pause or resume drawing |
| `livewall -autostart off` | Don't start with the desktop |
| `livewall -quit` | Stop the wallpaper |

Settings are kept in `~/.config/livewall/config.ini`, and problems are written to `~/.config/livewall/livewall.log`.

## Your own shaders

Shadertoy shaders work almost unchanged on Linux, because both use GLSL: save one with a `mainImage()` function as a `.glsl` file and run `livewall -set file.glsl`. See [`examples/plasma.glsl`](../examples/plasma.glsl) and [CUSTOM_SHADERS.md](CUSTOM_SHADERS.md).

## Build from source

```
sudo apt install build-essential pkg-config libwayland-dev wayland-protocols libegl-dev libgles-dev libx11-dev libmpv-dev
make -C linux
```

The result is `linux/build/livewall` (about 80 KB).

## Uninstall

1. `livewall -autostart off`, then `livewall -quit`
2. Delete the `livewall` file and `~/.config/livewall`
