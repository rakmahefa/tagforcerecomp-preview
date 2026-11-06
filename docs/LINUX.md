# The Linux launcher (preview)

This page is about running Tag Force Recomp on **Linux**: what the Linux release contains, what works today, and how it is built. For the Windows release, see the [README](../README.md).

The Linux release follows the shape of the [DQ8Recomp](https://github.com/Sinan-Karakaya/DQ8-Recompiled) launcher's releases, which this project's launcher takes its five-page flow from:

```
TagForceRecomp/
  tagforce-launcher   the launcher (SDL2 + Dear ImGui, no install)
  libSDL2-2.0.so.0    SDL2, bundled (zlib license); a system SDL2 is used if present
  payload/            everything the launcher works from
    source/           this repository, tracked files only
    payload.json      the release this payload belongs to
  README-LINUX.txt    the three-step start
```

One download, one executable, one folder - like the Windows zip, nothing to install beyond the tools the distribution already packages.

## Playing (once the runtime builds on Linux)

1. Download `TagForceRecomp-<version>-Linux-x86_64.tar.gz` from the [releases](../../releases) page and extract it anywhere.
2. Run `./tagforce-launcher` and drag your disc image (a `.iso` of **your own** European copy, ULES-00600) onto the window.
3. Walk the pages with **Continue**; press **Start building**, then **Play**.

The five pages:

| Page | What it does |
|---|---|
| **Your disc** | Reads the ISO: game id (ULES-00600), volume, size. A wrong disc is refused, with the reason. |
| **Tools** | Looks for g++, CMake, Ninja, Python 3 and FFmpeg, with versions; shows the one-line install command for your distribution. |
| **Options** | Where the game files go (default `~/TagForceRecomp`) and how many compile jobs to use. |
| **Build** | Copies the game files from your disc (`tools/game/extrair_jogo.py`), translates the game's code (`tools/translator/`), then compiles the runtime. Progress, a live log, cancel. |
| **Play** | Starts the game, once a Linux build of the runtime sits beside the launcher. |

Nothing from the game is downloaded or included anywhere: the payload carries source and tools only, and everything derived from your disc stays on your machine.

## What works today, honestly

**The runtime now builds and runs on Linux** (branch `linux-runtime`). The port keeps the game code single-source: `runtime/plat/` is a Win32 compatibility layer (POSIX + SDL2 + a software GDI) that every Windows-oriented file compiles against unchanged, and the genuinely platform-bound spots carry `#ifdef __linux__`. What it covers:

- the HLE kernel and the whole game pipeline (coroutines in SysV AMD64 assembly, the 1 GiB PSP address space through `mmap`, SIGSEGV-driven VRAM write tracking for the GPU renderer);
- **the TinyCC JIT**: `libtcc.so` (plus `libtcc1.a` and TinyCC's `include/`) replaces `libtcc.dll`; the translated modules compile and link in memory exactly as on Windows;
- the window and all the dark UI (launcher, message boxes, text input, mod panels) on SDL2 windows with a per-thread Win32-style message queue, GDI drawing and GDI+ antialiasing on an ARGB32 software canvas, and `stb_truetype` fonts (Segoe UI -> Liberation Sans / DejaVu Sans, MS Gothic -> the system CJK font);
- input: keyboard and mouse, XInput and DirectInput controllers through SDL2 game controllers;
- audio: the same pull pipeline as WASAPI, delivered by an SDL2 audio callback (PulseAudio/PipeWire/ALSA);
- renderers: the software one, and Vulkan through an Xlib surface (Direct3D 11 does not exist here; `gpu_select` refuses it and the game falls back);
- files: the ISO reader, installation, saves, logs, INI settings, mods as `plugin.so`;
- the translator runs with the system `python3` instead of the embedded one.

Still Windows-only (documented gaps): the launcher's update check (reported as unavailable, one log line), exclusive fullscreen (borderless is used), the DWM vblank pacing clock (the nominal 59.94 grid paces the frames), and the native file dialog falls back to a terminal prompt when there is no `zenity`.

With that, the pipeline of the launcher's Build page is no longer blocked at *Compile the runtime* once it invokes `tools/build_linux.sh`; **Play** starts `gen/linux/TagForceRecomp`. On Linux the runtime currently recognises the disc, checks the tools, copies the game files from the ISO into the workspace (`jogo/`), translates the game's code to C with the same Python translator the Windows release embeds, and then compiles the runtime.

The [screenshots](screenshots/launcher/) show every page, including the ones for states you should not meet.

## Building the runtime on Linux (developers)

```sh
sudo apt install g++ python3 libsdl2-dev libvulkan-dev libavcodec-dev libavformat-dev \
                 libavutil-dev libswscale-dev libswresample-dev libpng-dev \
                 libjpeg62-turbo-dev libstb-dev zlib1g-dev glslc   # or your distribution's names
# TinyCC (LGPL) from source: ./configure --prefix=... && make libtcc.so libtcc1.a tcc && make install
sh tools/build_linux.sh                  # -> gen/linux/TagForceRecomp
```

The build mirrors `tools/build.sh`: every `runtime/` and `decomp/` source except `gpu_d3d11.cpp`, compiled against `runtime/plat/` first in the include path (that is where `windows.h`, `gdiplus.h`, `dinput.h` and friends come from), linked with SDL2, FFmpeg, Vulkan, libpng, libjpeg and libtcc. `TF_SYSROOT=<dir>` points both the compile and the link at a non-system prefix (that is also how a self-contained package can be built). Next to the executable the runtime expects the same layout as the Windows zip: `tcc/` (libtcc, its headers and `libtcc1.a`), `include/psp_rt.h`, `tools/translator/` - and the game data comes from your ISO as on Windows.

Smoke tests used during the port (no game data needed): run with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` (launcher window, log session, update check, ISO picker), `--install`/`--verify` with the synthetic disc of `tools/launcher/test/make_test_iso.py` (ISO reading, game id, PSP structure check), and the JIT self-test of `TF_JIT_SELFTEST=1` once translated modules exist.

## Command line

```
tagforce-launcher [--repo DIR] [--disc ISO] [--workspace DIR] [--jobs N]
                  [--build | --preview DIR | --smoke | --screenshot FILE]
                  [--check-disc ISO]
```

`--build` runs the same build with no window, printing progress; `--check-disc` recognises an ISO and exits (used by the CI test); `--preview DIR` draws the eight fixed pages into PNGs (the screenshots in `docs/screenshots/launcher/` are made this way, on a machine with no game data at all); `--smoke` draws them without saving; `--screenshot FILE` saves the live window.

## Building the launcher (developers)

```
sudo apt install g++ libsdl2-dev git    # or your distribution's equivalents
sh tools/launcher/build.sh              # -> build/tagforce-launcher
python3 tools/launcher/package_linux.py # -> gen/linux/TagForceRecomp-<hash>-Linux-x86_64.tar.gz
```

`build.sh` is a plain g++ build, like the rest of the project's tooling. Dear ImGui v1.91.9 (MIT) is fetched once, pinned, into `tools/launcher/deps/imgui` (git-ignored); SDL2 (zlib license) comes from the system and is bundled beside the launcher in the package. The fonts are Liberation Sans (SIL OFL 1.1) from `tools/launcher/fonts/`. The tests the CI runs are in [`.github/workflows/linux-launcher.yml`](../.github/workflows/linux-launcher.yml): a synthetic ULES-00600 image (no game data - see `tools/launcher/test/make_test_iso.py`), the smoke test, and the preview screenshots.

The launcher's own code is in `tools/launcher/`:

| File | What it is |
|---|---|
| `launcher_main.cpp` | window, GL context, fonts, style, command line, screenshots |
| `launcher_app.cpp` | the five pages, sidebar, footer, background - the whole UI |
| `launcher_iso.cpp` | the minimal ISO9660 reader (same logic as `runtime/core/gameid.cpp`) |
| `launcher_tools.cpp` | the tool checks |
| `launcher_pipeline.cpp` | the build stages as child processes, with log and cancel |
| `launcher_png.cpp` | the self-contained PNG writer for screenshots |
| `package_linux.py` | assembles the release: launcher + libSDL2 + payload |
