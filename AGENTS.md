# DroidSource — Agent Context

## What this repo is

OBS Studio plugin that adds "DroidSource" as a native OBS source. Plugin spawns a patched [scrcpy](https://github.com/Genymobile/scrcpy) subprocess; subprocess tees the server→client raw H.264 packet stream (`[pts:u64 BE | flags:u8 | size:u32 BE | NAL]`) to a loopback TCP port. Plugin parses the packets, feeds NAL units to `libavcodec`, emits decoded frames via `obs_source_output_video()`.

Target platform: Windows first (user's primary OS). Linux/macOS later.

## Architecture

- Plugin: GPL-2.0-or-later (matches libobs).
- scrcpy: Apache-2.0, shipped as separate binary, not linked. Subprocess boundary keeps licenses separate.
- adb: bundled on Windows (matching scrcpy upstream). Linux/macOS use system package.
- No `libavformat` — raw packet framing is parsed directly. scrcpy's `sc_packet_merger` pattern replicated in `scrcpy-reader.c` (buffer CONFIG, prepend to next media packet).

## Repo layout

- `CMakeLists.txt`, `buildspec.json` — OBS plugin build (based on `obsproject/obs-plugintemplate`)
- `src/plugin-main.c` — module entry, registers `scrcpy_source_info`
- `src/scrcpy-source.c` — OBS source type: properties, ephemeral port, subprocess lifecycle
- `src/scrcpy-process.c` — cross-platform scrcpy subprocess spawn
- `src/scrcpy-reader.c` — TCP reader, packet merger, avcodec decode, frame emission
- `data/locale/en-US.ini` — i18n strings
- `data/bin/` — bundled scrcpy.exe + scrcpy-server + DLLs (gitignored, populated by CI/local install)
- `tests/` — pytest test suite (uv-managed). `wire/` = raw packet tests, `e2e/` = OBS WebSocket tests.
- `scrcpy/` — **git submodule**. Fork `https://github.com/wtarit/scrcpy` (origin), Genymobile upstream as secondary remote. Submodule pins tag `vX.Y.Z-rawstream.N`. Do not edit files in place — go through the fork.

## Build requirements (Windows)

- Visual Studio 2022 (MSVC v143).
- CMake ≥ 3.28.
- OBS Studio 31.1.1 headers (fetched by buildspec.json via obs-deps).
- Qt 6 (bundled via obs-deps).
- MSYS2 at `C:\msys64` — packages: `mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-sdl3 mingw-w64-x86_64-ffmpeg mingw-w64-x86_64-libusb mingw-w64-x86_64-gcc` (scrcpy ≥ 4.0 requires SDL3 + FFmpeg ≥ 8.1).
- `gh` CLI on PATH.

## Commands

- `git submodule update --init --recursive` after fresh clone.
- Build plugin (Windows, from repo root):
  ```powershell
  cmake --preset windows-x64
  cmake --build --preset windows-x64
  ```
- Build scrcpy binary:
  ```bash
  # MSYS2 MINGW64 shell
  cd scrcpy && meson setup builddir --buildtype=release -Dcompile_server=false -Dportable=true \
    && ninja -C builddir
  ```
- OBS plugin install dir on dev machine: `C:\ProgramData\obs-studio\plugins\droidsource\`. OBS ignores the AppData copy if the ProgramData one exists.
- Format, build, and ADB details: [CONTRIBUTING.md](CONTRIBUTING.md)
- Tests: [tests/README.md](tests/README.md)
- User-facing install/usage: [README.md](README.md)

## Useful upstream refs

- OBS plugin template: https://github.com/obsproject/obs-plugintemplate
- DistroAV (NDI → OBS plugin, architectural reference): https://github.com/DistroAV/DistroAV
- scrcpy source modules of interest: `app/src/demuxer.c`, `app/src/decoder.c`, `app/src/packet_merger.c`, `app/src/adb/`, `app/src/server.c`

## Conventions

- Windows paths: forward slashes in bash, backslashes only in PowerShell.
- Don't touch files under `scrcpy/` — submodule, pinned tag. Changes go through the fork.
- One OBS source type per file in `src/`.
- Active dev branch: `dev/rawstream`. PRs target `dev/rawstream` for now.
- Remote: `git@github.com:DLC86/DroidSource.git` (SSH).
- MSVC doesn't enable C11 atomics — use OBS `os_atomic_*` helpers on `volatile bool`.
