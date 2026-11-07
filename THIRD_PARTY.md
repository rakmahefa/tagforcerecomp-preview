# Third-party software and content

This project's own code and documentation are under the [MIT license](LICENSE). This repository contains **no game data**. The project builds on, links to, or interoperates with the following; each keeps its own license.

| Component | Use | License |
|---|---|---|
| [FFmpeg](https://ffmpeg.org/) 9.0.2 (`libavcodec`, `libavutil`) | decoding of H.264 movies and ATRAC3/3+ audio; loaded **dynamically** at run time. The release zip carries a **slim, LGPL-only build** made by `tools/build/build_ffmpeg_lite.sh` from the official source tarball (`./configure --disable-everything --enable-decoder=atrac3,atrac3p,h264 --disable-avformat ...`, no GPL components). You can replace the two DLLs with your own build of the same FFmpeg version. Corresponding source: https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz . The repository itself contains no FFmpeg code | LGPL-2.1+ |
| [winpthreads](https://www.mingw-w64.org/) (`libwinpthread-1.dll`) | runtime library of the FFmpeg DLLs, from MSYS2 | MIT / ZPL |
| [TinyCC](https://bellard.org/tcc/) (`libtcc.dll`, headers, `libtcc1-64.a`; release 0.9.27) | compiles the translated game code in memory at start-up; shipped unmodified in the release zip, downloaded from the official release by `tools/package_release.py` (source: https://repo.or.cz/tinycc.git). On Linux the release payload carries it pre-compiled (`prebuilt/tcc/`: `libtcc.so`, `libtcc1.a`, `include/`), built from the same official source by `tools/build_tcc_linux.sh` when `tools/launcher/package_linux.py` assembles the tarball (on the CI's oldest supported runner), loaded beside the game at run time (never linked into it); a git checkout builds it the same way on the player's machine instead | LGPL-2.1 |
| [SDL2](https://www.libsdl.org/) 2.x (the system's `libsdl2`, and `libSDL2-2.0.so.0` bundled beside the Linux launcher) | window, input and the OpenGL context of the **Linux launcher**, and the window, input and audio of the **Linux runtime**; the bundled library is copied unmodified from the distribution's package by `tools/launcher/package_linux.py` | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) v1.91.9 (pinned by `tools/launcher/build.sh`, fetched at build time, never committed) | draws the **Linux launcher**'s five pages | MIT |
| [Liberation Sans](https://github.com/liberationfonts) (in `tools/launcher/fonts/`) | the **Linux launcher**'s typeface, regular and bold | SIL OFL 1.1 |
| [stb](https://github.com/nothings/stb) (`stb_image.h` v2.30, `stb_truetype.h` v1.26, vendored in `runtime/plat/stb/`) | image decoding and font rasterisation of the **Linux runtime**'s GDI/GDI+ layer | public domain / MIT |
| [Python](https://www.python.org/) embeddable package (3.11) | runs the Allegrex -> C translator on the player's machine; shipped unmodified in the release zip | PSF License |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) (`llama-server`) | runs the small language model of the Better AI mod; distributed with the **mod package**, not in this repository | MIT |
| Qwen2.5-0.5B-Instruct (Alibaba Cloud) | base model that the Better AI mod fine-tunes with LoRA; the fine-tuned weights are distributed with the mod package, not in this repository | Apache-2.0 |
| [PPSSPP](https://www.ppsspp.org/) | **read-only reference** for hardware behavior and for the texture-pack format; **no code is copied** | GPL-2.0+ |
| Microsoft Windows APIs, Direct3D 11, [Vulkan](https://www.vulkan.org/) | rendering and platform | platform / Khronos terms |
| XXH64 (Yann Collet's public algorithm) | hash used by PPSSPP-format texture packs; **own implementation** in `runtime/core/xxh.h`, no xxHash code copied | — |
| PPSSPP-format HD texture packs (MikeyMK, Filipe Ramos), character sprites (Artknightt), [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) upscaling | **not in the repository**; included in the Enhanced Textures release download with credit (README, pack `README.txt`, `mod.txt`) | their authors' terms |

## Trademarks and game content

Yu-Gi-Oh! and Tag Force are trademarks of Konami and its licensors. This project is **not affiliated with or endorsed by Konami**.
You must own a legitimate copy of the game. Card names and numbers appear in documentation and tools only as references to your
own copy's data; the repository does not ship the card database, deck lists, images, audio or video.
