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

The runtime (`runtime/`) is written for Windows (Win32, Direct3D 11, Vulkan, TinyCC). On Linux, the launcher currently:

- recognises the disc - really, from the ISO itself;
- checks the tools;
- **copies the game files** from your ISO into the workspace (`jogo/`);
- **translates the game's code** to C with the same Python translator the Windows release embeds;
- and stops at *Compile the runtime*, saying so plainly. When the runtime gains a Linux build, the same stage builds it, and **Play** starts the game - no other change needed.

So the Linux release is the whole pipeline around the game, ready for the runtime's port. The [screenshots](screenshots/launcher/) show every page, including the ones for states you should not meet.

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
