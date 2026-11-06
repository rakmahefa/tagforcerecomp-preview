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

## Playing

1. Download `TagForceRecomp-<version>-Linux-x86_64.tar.gz` from the [releases](../../releases) page and extract it anywhere.
2. Run `./tagforce-launcher` and drag your disc image (a `.iso` of **your own** European copy, ULES-00600) onto the window.
3. Walk the pages with **Continue**; press **Start building**, then **Play**. The Build page copies your game files, translates the game's code and compiles it (the Tools page lists the packages your distribution needs); Play starts the result.

The five pages:

| Page | What it does |
|---|---|
| **Your disc** | Reads the ISO: game id (ULES-00600), volume, size. A wrong disc is refused, with the reason. |
| **Tools** | Looks for what the build needs - g++, Python 3, SDL2, the FFmpeg/PNG/JPEG libraries, Vulkan - with versions; shows the one-line install command for your distribution. The Vulkan shaders and TinyCC ride pre-compiled in the payload (`prebuilt/`), so neither the shader compiler nor a TinyCC build is the player's problem. |
| **Options** | Where the game files go (default `~/TagForceRecomp`) and how many compile jobs to use. |
| **Build** | Copies the game files from your disc (`tools/game/extrair_jogo.py`), translates the game's code (`tools/translator/`), then compiles the game (`tools/build_linux.sh`, one `[n/57]` line per source as the progress bar). Progress, a live log, cancel. |
| **Play** | Starts the game the Build stage left in `<workspace>/source`, with the game data from `<workspace>/jogo`. |

Nothing from the game is downloaded or included anywhere: the payload carries source and tools only, and everything derived from your disc stays on your machine.

## What works today, honestly

**The runtime now builds and runs on Linux** (branch `linux-runtime`). The port keeps the game code single-source: `runtime/plat/` is a Win32 compatibility layer (POSIX + SDL2 + a software GDI) that every Windows-oriented file compiles against unchanged, and the genuinely platform-bound spots carry `#ifdef __linux__`. What it covers:

- the HLE kernel and the whole game pipeline (coroutines in SysV AMD64 assembly, the 1 GiB PSP address space through `mmap`, SIGSEGV-driven VRAM write tracking for the GPU renderer);
- **the TinyCC JIT**: `libtcc.so` (plus `libtcc1.a` and TinyCC's `include/`) replaces `libtcc.dll`; the translated modules compile and link in memory exactly as on Windows. The release payload carries TinyCC ready-made in `prebuilt/tcc/` (built at package time by the same `tools/build_tcc_linux.sh`, on the oldest supported glibc, so nothing is compiled or downloaded on the player's machine); a plain git checkout builds it from the official source once instead. `libtcc1.a`, which every module link needs, is searched in the bundle, next to the loaded `libtcc.so` and in the usual distribution spots (`/usr/lib/tcc` on Arch) - no hand-made symlink. Only the tested TinyCC 0.9.27 is ever bundled: `build_linux.sh` refuses a system `libtcc.so` of any other version (Arch's 0.9.28rc compiled the game with a code generator it was never verified with, and a first real boot crashed inside the recompiled BOOT); the log's first `[jit]` line names the version in use;
- the window and all the dark UI (launcher, message boxes, text input, mod panels) on SDL2 windows with a per-thread Win32-style message queue, GDI drawing and GDI+ antialiasing on an ARGB32 software canvas, and `stb_truetype` fonts (Segoe UI -> Liberation Sans / DejaVu Sans, MS Gothic -> the system CJK font);
- input: keyboard and mouse, XInput and DirectInput controllers through SDL2 game controllers;
- audio: the same pull pipeline as WASAPI, delivered by an SDL2 audio callback (PulseAudio/PipeWire/ALSA);
- renderers: the software one, and Vulkan through an Xlib surface (Direct3D 11 does not exist here; `gpu_select` refuses it and the game falls back);
- files: the ISO reader, installation, saves, logs, INI settings, mods as `plugin.so`;
- the translator runs with the system `python3` instead of the embedded one.

Still Windows-only (documented gaps): the launcher's update check (reported as unavailable, one log line), exclusive fullscreen (borderless is used), the DWM vblank pacing clock (the nominal 59.94 grid paces the frames), and the native file dialog falls back to a terminal prompt when there is no `zenity`.

With that, the launcher's Build page runs the whole pipeline: the disc is recognised, the tools are checked, the game files are copied from the ISO into the workspace (`jogo/`), the game's code is translated to C with the same Python translator the Windows release embeds, and the runtime is compiled with `tools/build_linux.sh`. **Play** starts `<workspace>/source/TagForceRecomp` with `--data <workspace>/jogo`.

The [screenshots](screenshots/launcher/) show every page, including the ones for states you should not meet.

## Building the runtime on Linux (developers)

```sh
sudo apt install build-essential python3 pkg-config bzip2 libsdl2-dev libavcodec-dev \
                 libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev \
                 libjpeg-dev libvulkan-dev   # or your distribution's names
sh tools/build_linux.sh                  # -> gen/linux/TagForceRecomp, and a ready-to-run
                                          #    install in the repository root
```

The build checks its dependencies first and, when something is missing, prints the exact package line to install. The Vulkan shaders ship pre-compiled (`prebuilt/gpu_spv.h`, regenerated by `tools/build/spv2h.py`); `glslc` (the shaderc package) is only needed to recompile them after editing one. TinyCC (LGPL) is the same: a release payload carries the tested 0.9.27 ready-made in `prebuilt/tcc/`, and `build_linux.sh` prefers that copy; a git checkout - which has no binaries - compiles `libtcc.so` once from the official 0.9.27 source into `gen/linux/tcc/` (`tools/build_tcc_linux.sh`, needs `make` and `curl` or `wget`), and later builds reuse it. `TF_TCC=<dir>` points at an existing `libtcc.so`. The stb headers (`stb_image.h`, `stb_truetype.h`) are vendored in `runtime/plat/stb/`.

The build mirrors `tools/build.sh`: every `runtime/` and `decomp/` source except `gpu_d3d11.cpp`, compiled against `runtime/plat/` first in the include path (that is where `windows.h`, `gdiplus.h`, `dinput.h` and friends come from), linked with SDL2, FFmpeg, Vulkan, libpng and libjpeg through `pkg-config` (TinyCC is loaded at run time, never linked in). `TF_SYSROOT=<dir>` points both the compile and the link at a non-system prefix (that is also how a self-contained package can be built). Like the Windows build, the repository root becomes a ready-to-run install: `TagForceRecomp`, `tcc/` (libtcc and its headers) and `include/psp_rt.h` beside each other (all git-ignored); `jogo/` and `gen/` are made from your ISO as on Windows.

Smoke tests used during the port (no game data needed): run with `SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy` (launcher window, log session, update check, ISO picker), `--install`/`--verify` with a synthetic disc, and `TF_JIT_SELFTEST=1` once translated modules exist. The JIT's object cache (`gen/jitcache/`) round-trips: the runtime asks the loaded libtcc which output type writes a relocatable ELF object (after 0.9.27 the constants were renumbered), validates every object it writes and falls back to compiling in memory when the disk is nearly full.

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
