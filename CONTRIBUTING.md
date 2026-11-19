# Contributing to livewall

Thanks for wanting to help! livewall aims to be **the lightest live wallpaper there is**. Every change is judged first by what it costs a user who just wants a nice desktop.

## Ways to help

- **Report a bug.** Open an issue with:
  - your Windows version (`winver`);
  - your GPU(s);
  - what you expected and what happened;
  - the last lines of `%APPDATA%\livewall\livewall.log`.
- **Share a shader.** Write a wallpaper (see [docs/CUSTOM_SHADERS.md](docs/CUSTOM_SHADERS.md)) and open a pull request adding it to `examples/`. Good built-in candidates are calm, dark enough for icons to stay readable, and cheap.
- **Test on your machine.** Windows 10, pre-24H2 Windows 11, multi-monitor and AMD GPUs are all under-tested. See [docs/STATUS.md](docs/STATUS.md).
- **Port it.** Linux, macOS and Android are on the roadmap in the README. Open an issue first so we can agree on the shared core.

## Building

Install Visual Studio 2022 (or the Build Tools) with **Desktop development with C++**, then run:

```
build.bat
build\livewall.exe -snapshot aurora test.png
```

`-snapshot` renders any wallpaper to a PNG without touching your desktop, which is handy for checking a change. `tools\measure.ps1` measures memory, CPU and GPU use.

## Rules for code

- **Keep the engine lean.** The engine (the process that stays running) must not allocate per frame, poll faster than needed, or load UI libraries. UI belongs in `settings.cpp`, which runs only while the settings window is open.
- **No C runtime.** The exe links without the CRT. Use the helpers in `src/util.c`, keep stack buffers under 4 KB, and in C++ avoid exceptions, RTTI, the STL and global constructors.
- **Measure.** If a change touches the engine, include before/after numbers from `tools\measure.ps1` in the pull request.
- **Keep the docs true.** Update [docs/STATUS.md](docs/STATUS.md) when something becomes verified or broken. Add facts you discovered about Windows to [docs/FINDINGS.md](docs/FINDINGS.md).
- Match the existing style: 4 spaces, short comments that explain *why*.

## Pull requests

1. Fork the repo and create a branch.
2. Keep each pull request to one change, with a clear description of what it does and how you tested it.
3. By contributing, you agree your work is released under the [MIT license](LICENSE).
