#!/bin/sh
# The Linux build of the runtime (the counterpart of tools/build.sh, which stays the Windows one).
#
# The runtime is single-source: runtime/plat/ holds a Win32 compatibility layer (POSIX + SDL2 +
# software GDI) that every Windows-oriented file compiles against unchanged. Direct3D 11 has no
# Linux counterpart (gpu_select refuses it and the game falls back to Vulkan or the software
# renderer), and the few genuinely platform-bound spots (coroutines, the TinyCC JIT, audio, the
# Vulkan surface, image codecs, the translator's Python, mod plugins) carry #ifdef __linux__.
#
# Usage: sh tools/build_linux.sh     Variables: OPT=-O1 (default -O2), JOBS=n, OUT=path
#
# Dependencies (Debian/Ubuntu names): g++ make python3 libsdl2-dev libvulkan-dev
#   libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
#   libpng-dev libjpeg62-turbo-dev libstb-dev zlib1g-dev   (+ the game data, from your ISO)
set -e
cd "$(dirname "$0")/.."
SYSROOT="${TF_SYSROOT:-/usr}"
CXXFLAGS_COMMON="-std=c++17 -fno-strict-aliasing -fwrapv ${OPT:--O2} -fPIC -g1 -DPSP_TRACE \
  -I runtime/plat -I runtime -I runtime/core -I runtime/hle -I runtime/gpu -I runtime/ui -I runtime/audio -I runtime/mods -I . \
  -I $SYSROOT/usr/include -I $SYSROOT/include"
if [ -n "$TF_SYSROOT" ] && [ "$TF_SYSROOT" != "/usr" ]; then
  CXXFLAGS_COMMON="$CXXFLAGS_COMMON -I $TF_SYSROOT/usr/include/x86_64-linux-gnu"
fi
export PKG_CONFIG_PATH="${TF_PKG_CONFIG_PATH:-$SYSROOT/usr/lib/x86_64-linux-gnu/pkgconfig:$SYSROOT/usr/share/pkgconfig:$PKG_CONFIG_PATH}"
INCSDL2="$SYSROOT/usr/include/SDL2"
INCSTB="$SYSROOT/usr/include/stb"
INCMACH="$SYSROOT/usr/include/x86_64-linux-gnu"
CXXFLAGS_COMMON="$CXXFLAGS_COMMON -I $INCSDL2 -I $INCSTB -I $INCMACH"
LIBS="$(pkg-config --libs sdl2 libavcodec libavformat libavutil libswscale libswresample libpng libjpeg 2>/dev/null) -lvulkan -ltcc -lz -lpthread -ldl -lm"
if [ -n "$TF_SYSROOT" ] && [ "$TF_SYSROOT" != "/usr" ]; then
  LIBS="-L $TF_SYSROOT/usr/lib/x86_64-linux-gnu -L $TF_SYSROOT/usr/lib -Wl,-rpath,$TF_SYSROOT/usr/lib/x86_64-linux-gnu -Wl,-rpath,$TF_SYSROOT/usr/lib $LIBS"
fi

OD=gen/linux/obj; mkdir -p $OD

# the Vulkan shaders first (gpu_vulkan.cpp includes gen/gpu_spv.h)
if [ -n "$TF_SYSROOT" ] && [ -x "$TF_SYSROOT/usr/bin/glslc" ]; then export PATH="$TF_SYSROOT/usr/bin:$PATH" LD_LIBRARY_PATH="$TF_SYSROOT/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"; fi
if ! command -v glslc >/dev/null 2>&1 && [ ! -f gen/gpu_spv.h ]; then
  echo "glslc not found and gen/gpu_spv.h missing (install shaderc/glslc or copy it from another build)"; exit 1
fi
if command -v glslc >/dev/null 2>&1; then python3 tools/build/spv2h.py || exit 1; fi
GAMEID=ULES-00600
APPVER=${APPVER:-$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' CHANGELOG.md | head -1)}
STAMP="${OPT:--O2} $GAMEID $APPVER linux"
[ "$(cat $OD/flags.txt 2>/dev/null)" = "$STAMP" ] || { rm -f $OD/*.o $OD/*.d; echo "$STAMP" > $OD/flags.txt; }
export CXXOPT="${OPT:--O2}" GAMEID APPVER

# every runtime and decomp source, except the Direct3D 11 backend; the shim is added
CPPSRCS=$(ls runtime/plat/*.cpp runtime/*/*.cpp decomp/decomp.cpp decomp/*/*.cpp | grep -v 'gpu_d3d11.cpp')

TODO=""
for s in $CPPSRCS; do
  o=$OD/$(echo "${s%.cpp}" | tr '/' '_').o
  need=0
  if [ ! -f "$o" ] || [ "$s" -nt "$o" ]; then need=1
  elif [ -f "${o%.o}.d" ]; then
    for h in $(sed -e 's/^[^:]*://' -e 's/\\$//' "${o%.o}.d" | tr ' ' '\n' | grep -E '^(runtime|decomp|gen)/' 2>/dev/null); do
      if [ ! -e "$h" ] || [ "$h" -nt "$o" ]; then need=1; break; fi
    done
  fi
  [ $need = 1 ] && TODO="$TODO $s:$o"
done

if [ -n "$TODO" ]; then
  echo "$TODO" | tr ':' '\n' | xargs -P ${JOBS:-$(nproc)} -n2 sh -c '
    s=$0; o=$1
    g++ '"$CXXFLAGS_COMMON"' "-DTF_GAME_ID=\"$GAMEID\"" "-DTF_APP_VERSION=\"${APPVER:-0.0.0}\"" -MMD -c "$s" -o "$o" 2> "${o%.o}.err" || { cat "${o%.o}.err"; rm -f "${o%.o}.err"; exit 1; }
    [ -s "${o%.o}.err" ] && cat "${o%.o}.err" 1>&2
    rm -f "${o%.o}.err"
  '
fi


EXE="${OUT:-gen/linux/TagForceRecomp}"
mkdir -p "$(dirname "$EXE")"
g++ -o "$EXE" $OD/*.o $LIBS -Wl,-rpath,'$ORIGIN/../lib'
echo "built: $EXE"
