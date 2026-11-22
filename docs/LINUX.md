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
| `livewall -lock` | Lock now (see [Lock screen](#lock-screen)) |
| `livewall -lock-set nightsky` | The lock screen wallpaper (`same` = as the desktop) |
| `livewall -lock-picture` | A still picture of it, for lockers that take a picture |

Settings are kept in `~/.config/livewall/config.ini`, and problems are written to `~/.config/livewall/livewall.log`.

## Lock screen

livewall never asks for or checks your password. A real screen locker does that, and livewall only draws the picture behind it.

**Live wallpaper on the lock screen** (Sway, Hyprland, river, niri, labwc and other desktops with the Wayland lock protocol):

1. Install **swaylock-plugin**, a version of swaylock that can show an animated background. Arch: `swaylock-plugin` from the AUR. Other systems: build it from [its source](https://github.com/mstoeckl/swaylock-plugin).
2. Choose the lock wallpaper, or leave it the same as the desktop:
   ```
   livewall -lock-set nightsky       # or a file; "same" follows the desktop
   ```
3. Lock with `livewall -lock`. To lock when you step away, let your idle tool run it:
   ```
   # Sway (in ~/.config/sway/config)
   exec swayidle -w timeout 300 'livewall -lock -f' before-sleep 'livewall -lock -f'
   ```
   ```
   # Hyprland (in ~/.config/hypr/hypridle.conf)
   general {
       lock_cmd = pidof swaylock-plugin || livewall -lock -f
       before_sleep_cmd = loginctl lock-session
   }
   listener {
       timeout = 300
       on-timeout = loginctl lock-session
   }
   ```

While the screen is locked, the desktop wallpaper stops drawing (the desktop is hidden), so only the lock screen one runs. When you unlock, it closes by itself.

**Other lockers** (swaylock, hyprlock, KDE, X11 desktops): `livewall -lock` uses your desktop's own lock screen. To give it the same look, make a still picture of the lock wallpaper and point your locker at it:

```
livewall -lock-picture                       # writes ~/.cache/livewall/lock.png
swaylock -i ~/.cache/livewall/lock.png       # swaylock
background { path = ~/.cache/livewall/lock.png }   # hyprlock.conf
```

Add a size if your screen isn't 1920x1080: `livewall -lock-picture ~/lock.png 2560x1440`.

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
