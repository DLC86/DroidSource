# Contributing

Developer guide for building, formatting, and testing DroidSource.

## Architecture

The plugin spawns a patched scrcpy subprocess per source instance. scrcpy tees the raw video packet stream to a loopback TCP port; the plugin parses packets, and pushes decoded frames into OBS.

## Repo layout

| Path                    | Purpose                                                            |
| ----------------------- | ------------------------------------------------------------------ |
| `src/plugin-main.c`     | Module entry, registers `scrcpy_source_info`                       |
| `src/scrcpy-source.c`   | OBS source: properties, port, subprocess lifecycle                 |
| `src/scrcpy-process.c`  | Cross-platform scrcpy subprocess spawn                             |
| `src/scrcpy-reader.c`   | TCP reader, packet merger, avcodec decode, frame output            |
| `data/locale/en-US.ini` | i18n strings                                                       |
| `data/bin/`             | Bundled scrcpy binary (gitignored; populated by CI or local build) |
| `tests/`                | pytest suite — see [tests/README.md](tests/README.md)              |
| `scrcpy/`               | Link to patched scrcpy                                             |

## Build requirements (Windows)

See

- [scrcpy's build guide](https://github.com/Genymobile/scrcpy/blob/master/doc/build.md)
- [OBS plugin template quick start](https://github.com/obsproject/obs-plugintemplate/wiki/Quick-Start-Guide).

## Build from source

### 1. Clone

```bash
git clone git@github.com:DLC86/DroidSource.git
cd DroidSource
git submodule update --init --recursive
```

### 2. Build the scrcpy subprocess binary

> Note: For windows use MSYS2 MINGW64 shell

```bash
cd scrcpy
meson setup builddir --buildtype=release -Dcompile_server=false -Dportable=true
ninja -C builddir
cd ..
```

Use `-Dportable=true` so scrcpy finds `scrcpy-server` next to the executable.

Output: `scrcpy/builddir/app/scrcpy.exe` + DLLs. Copy these (plus `scrcpy-server` and `adb` binary) into `data/bin/` before building the plugin, or the binary won't be included when installing the plugin.

### 3. Build the OBS plugin

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
```

### 4. Install to OBS

```powershell
cmake --install build_x64 --config RelWithDebInfo
```

Restart OBS so it pickup new plugins.

## Formatting

CI checks formatting (`.github/workflows/check-format.yaml`).

- **clang-format** for C sources (`src/*.c`, `src/*.h`)
- **gersemi** for CMake (`CMakeLists.txt`)

Install clang-format (Windows):

```powershell
winget install -e --id LLVM.ClangFormat
```

Format the codebase:

```powershell
Get-ChildItem src -Recurse -Include *.c,*.h | ForEach-Object { clang-format -i $_.FullName }
uvx gersemi@0.21.0 -i .\cmake\ .\CMakeLists.txt
```

## Testing

See [tests/README.md](tests/README.md).

## Building Windows Installer Locally

- Ensure that scrcpy, adb and relevent .dll is in `data/bin` folder. You can build patched version of scrcpy or download [prebuilt binary](https://github.com/wtarit/scrcpy/releases).

```Powershell
# build + install
cmake --build --preset windows-x64 --config RelWithDebInfo
cmake --install build_x64 --prefix "$PWD/release/RelWithDebInfo" --config RelWithDebInfo

# flatten droidsource/ into Package (what iscc expects)
New-Item -ItemType Directory -Force -Path release/Package | Out-Null

Copy-Item -Path release/RelWithDebInfo/droidsource/* -Destination release/Package -Recurse -Force

# compile the installer
iscc .\build_x64\installer-windows.iss /O"$PWD\release"

# cleanup
Remove-Item .\release\Package -Recurse -Force
```
