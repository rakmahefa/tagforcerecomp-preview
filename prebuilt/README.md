# prebuilt/ - what the player's machine must NOT need to build

Two pieces of the game would otherwise need a compiler or a download at Build
time, on the player's own machine. They are carried ready-made instead:

- **`gpu_spv.h`** - the Vulkan shaders (`runtime/gpu/shaders/*.vert`/`*.frag`),
  already compiled to SPIR-V. With this file present, `glslc` (the shaderc
  package) is not needed at all: `tools/build_linux.sh` copies it to
  `gen/gpu_spv.h` when no `glslc` is installed, and regenerates from the
  sources when one is (a developer editing a shader still gets the fresh
  result). To refresh it after changing a shader:

      GLSLC=glslc python3 tools/build/spv2h.py && cp gen/gpu_spv.h prebuilt/

- **`tcc/`** - TinyCC 0.9.27 (`libtcc.so`, `libtcc1.a` and TinyCC's `include/`),
  the JIT that compiles the translated game code at run time. Not in git: it is
  a binary, built for the oldest supported glibc at package time by
  `tools/build_tcc_linux.sh` (the official 0.9.27 source, fetched once) into
  `payload/source/prebuilt/tcc/` - see `tools/launcher/package_linux.py`.
  `tools/build_linux.sh` prefers it over compiling TinyCC on the player's
  machine; a plain git checkout (no payload) simply builds from source as
  before.

A stale `prebuilt/gpu_spv.h` would silently ship old shaders: after editing
anything under `runtime/gpu/shaders/`, regenerate it (the command above) and
commit the result together with the shader.
