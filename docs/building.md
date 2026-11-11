# Building Cord

## Prerequisites

| Requirement | Why | Check |
|---|---|---|
| Visual Studio 2022 or 2026, **Desktop development with C++** | MSVC, the Windows SDK and Ninja | `cl` in a Developer Prompt |
| CMake 3.28+ | build system | `cmake --version` |
| Git | fetching libobs | `git --version` |
| OBS Studio 30+ | to run and test the plugin | - |

> The **Desktop development with C++** workload is the one people miss.
> Installing Visual Studio without it gives you an IDE with no C++ compiler and
> no Windows SDK, and CMake fails at the configure step with "No CMAKE_C_COMPILER
> could be found".
>
> To add it: Visual Studio Installer -> Modify -> check *Desktop development
> with C++* -> Modify. Roughly 7 GB.
>
> From the command line:
> ```
> winget install --id Microsoft.VisualStudio.2022.Community --override "--quiet --add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
> ```

OBS Studio itself:

```
winget install --id OBSProject.OBSStudio
```

## Fetch dependencies

```powershell
powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1
```

This populates:

- `deps/` - libobs, FFmpeg and Qt6 (the prebuilt bundles OBS builds against)
- `data/bin/` - `adb.exe` and the `scrcpy-server` jar

Neither directory is in git. `data/bin/VERSION.json` records which scrcpy
version was pinned.

For a runtime-only refresh (after changing the pinned scrcpy version):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/fetch-deps.ps1 -SkipBuildDeps
```

## Configure and build

```
cmake --preset windows-x64
```

```
cmake --build --preset windows-x64
```

If the preset's generator does not match your Visual Studio version, edit
`CMakePresets.json` (`"generator"`): `Visual Studio 17 2022` or
`Visual Studio 18 2026`.

## Install into OBS

```
install.bat
```

That finds OBS Studio (from the installer's registry key, so Steam and winget
builds too) and Streamlabs Desktop, asks for administrator rights only if the
target needs them, and copies the plugin in. `install.bat /?` lists the
options - `/obs`, `/slobs`, `/dir "path"` for an install it cannot find, and
`/uninstall`.

Or do it by hand. The plugin goes into OBS's own install directory, in the
layout OBS uses for its bundled plugins:

```
cmake --install build --prefix "D:/path/to/OBS Studio"
```

That lands:

```
<OBS>\obs-plugins\64bit\cord.dll
<OBS>\data\obs-plugins\cord\locale\en-US.ini
<OBS>\data\obs-plugins\cord\bin\{adb.exe,scrcpy-server,...}
```

The second path is what `obs_module_file()` resolves against, which is how the
plugin finds the bundled adb and the device server at runtime.

An OBS update overwrites `obs-plugins\64bit`, so re-run `install.bat` after
one. For Streamlabs Desktop the same three paths sit under

```
<Streamlabs>\resources\app.asar.unpacked\node_modules\obs-studio-node\
```

which is the libobs Streamlabs bundles; `install.bat` handles it.

Restart OBS. **Phone (Cord)** appears in the source **+** menu.

## Verify the load

OBS writes a log per run: **Help -> Log Files -> View Current Log**. Look for:

```
[cord] loaded version 0.1.0
```

If the plugin is missing from the source list, the log will say why - almost
always a missing dependency DLL (`avcodec`, `avutil`) next to the plugin, or an
architecture mismatch.

## Bumping the pinned scrcpy version

The device-server protocol is **not stable** across scrcpy releases, and
`src/device/scrcpy-proto.h` is transcribed from one specific version. To bump:

1. Change `$ScrcpyVersion` in `scripts/fetch-deps.ps1`.
2. Change `CORD_SCRCPY_VERSION` in `src/device/scrcpy-proto.h` to match. The
   device server compares this string against its own and refuses to start if
   they differ.
3. Re-read these three files in the new scrcpy source and update the header to
   match:
   - `app/src/server.c` - launch arguments, socket order, device-info header
   - `app/src/demuxer.c` - codec ids, packet framing
   - `server/src/main/java/com/genymobile/scrcpy/Options.java` - option names
4. Re-run `fetch-deps.ps1 -SkipBuildDeps`, rebuild, and test on a real phone.

Skipping step 3 produces a session that connects and then reads nonsense.
