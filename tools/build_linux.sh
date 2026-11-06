#!/bin/sh
# The Linux build of the runtime (the counterpart of tools/build.sh, which stays the Windows one).
#
# The runtime is single-source: runtime/plat/ holds a Win32 compatibility layer (POSIX + SDL2 +
# software GDI) that every Windows-oriented file compiles against unchanged. Direct3D 11 has no
# Linux counterpart (gpu_select refuses it and the game falls back to Vulkan or the software
# renderer), and the few genuinely platform-bound spots (coroutines, the TinyCC JIT, audio, the
# Vulkan surface, image codecs, the translator's Python, mod plugins) carry #ifdef __linux__.
#
# Usage: sh tools/build_linux.sh     Variables: OPT=-O1 (default -O2), JOBS=n, OUT=path,
#   TF_SYSROOT=path (dependencies under a prepared prefix, the developer/packaging setup;
#   default: the system's own packages), TF_TCC=path (a folder with a libtcc.so to bundle;
#   default: compile it from the official source, once).
#
# Dependencies (Debian/Ubuntu names): build-essential python3 pkg-config glslc bzip2
#   libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev
#   libpng-dev libjpeg-dev libvulkan-dev   (+ the game data, from your ISO). TinyCC (LGPL) is
#   not packaged as a shared library: the first build compiles it from the official source.
#   The stb headers are vendored in runtime/plat/stb/.
set -e
cd "$(dirname "$0")/.."

PKGS="sdl2 libavcodec libavformat libavutil libswscale libswresample libpng libjpeg vulkan"

# ------------------------------------------------------------------ the dependency roots
S=""
if [ -n "$TF_SYSROOT" ] && [ "$TF_SYSROOT" != "/usr" ]; then
    S="$TF_SYSROOT"
    export PKG_CONFIG_PATH="${TF_PKG_CONFIG_PATH:-$S/usr/lib/x86_64-linux-gnu/pkgconfig:$S/usr/lib/pkgconfig:$S/usr/share/pkgconfig:${PKG_CONFIG_PATH:-}}"
    if [ -x "$S/usr/bin/glslc" ]; then PATH="$S/usr/bin:$PATH"; export PATH; fi
    export LD_LIBRARY_PATH="$S/usr/lib/x86_64-linux-gnu:${LD_LIBRARY_PATH:-}"
    EXTRA_INC="-I $S/usr/include -I $S/include -I $S/usr/include/x86_64-linux-gnu -I $S/usr/include/SDL2"
    LIBPATHS="-L $S/usr/lib/x86_64-linux-gnu -L $S/usr/lib"
    RPATHS="-Wl,-rpath,$S/usr/lib/x86_64-linux-gnu -Wl,-rpath,$S/usr/lib"
    PKGCFLAGS=""   # the prefix's .pc files still say prefix=/usr; the includes above are explicit
    PKGLIBS="$(pkg-config --libs-only-l $PKGS)"
else
    # the system's own packages; pkg-config resolves include and library paths
    if [ -n "$TF_PKG_CONFIG_PATH" ]; then export PKG_CONFIG_PATH="$TF_PKG_CONFIG_PATH"; fi
    EXTRA_INC="-I /usr/include/x86_64-linux-gnu"
    LIBPATHS=""
    RPATHS=""
    PKGCFLAGS="$(pkg-config --cflags $PKGS)"
    PKGLIBS="$(pkg-config --libs $PKGS)"
fi

# ------------------------------------------------------------------ the dependency check
MISSING=""
for c in g++ python3 pkg-config; do command -v $c >/dev/null 2>&1 || MISSING="$MISSING $c"; done
command -v glslc >/dev/null 2>&1 || [ -f gen/gpu_spv.h ] || MISSING="$MISSING glslc(the shader compiler)"
for m in $PKGS; do pkg-config --exists $m 2>/dev/null || MISSING="$MISSING $m"; done
if [ -n "$MISSING" ]; then
    echo "build_linux.sh: missing:$MISSING" 1>&2
    echo "  Debian/Ubuntu: sudo apt install build-essential python3 pkg-config glslc bzip2 libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev libswscale-dev libswresample-dev libpng-dev libjpeg-dev libvulkan-dev" 1>&2
    echo "  Fedora:        sudo dnf install gcc-c++ python3 pkgconf-pkg-config glslc bzip2 SDL2-devel ffmpeg-devel libpng-devel libjpeg-devel vulkan-headers vulkan-loader-devel" 1>&2
    exit 1
fi

# ------------------------------------------------------------------ TinyCC (libtcc.so, LGPL)
# The JIT loads libtcc.so beside the game at run time (never linked into it). The distributions
# package a static libtcc.a only, so the shared library is built from the official source once.
TCCDIR=""
for d in "${TF_TCC:-}" gen/linux/tcc "$S/usr/lib" /usr/lib/x86_64-linux-gnu /usr/lib64 /usr/local/lib; do
    if [ -n "$d" ] && [ -f "$d/libtcc.so" ]; then TCCDIR="$d"; break; fi
done
if [ -z "$TCCDIR" ]; then
    sh tools/build_tcc_linux.sh gen/linux/tcc
    TCCDIR=gen/linux/tcc
fi
if [ "$TCCDIR" != "gen/linux/tcc" ]; then
    mkdir -p gen/linux/tcc
    cp -L "$TCCDIR"/libtcc.so* gen/linux/tcc/
    if [ -f "$TCCDIR/libtcc1.a" ]; then cp -u "$TCCDIR/libtcc1.a" gen/linux/tcc/; fi
    TCCINC=""
    if [ -d "$TCCDIR/include" ]; then TCCINC="$TCCDIR/include"; elif [ -d "$TCCDIR/tcc/include" ]; then TCCINC="$TCCDIR/tcc/include"; fi
    if [ -n "$TCCINC" ]; then mkdir -p gen/linux/tcc/include; cp -ru "$TCCINC/." gen/linux/tcc/include/; fi
fi

# the one header the generated code includes, where the JIT looks for it
mkdir -p gen/linux/include
cp -u runtime/core/psp_rt.h gen/linux/include/

# ------------------------------------------------------------------ the compile
CXXFLAGS_COMMON="-std=c++17 -fno-strict-aliasing -fwrapv ${OPT:--O2} -fPIC -g1 -DPSP_TRACE \
  -I runtime/plat/stb -I runtime/plat -I runtime -I runtime/core -I runtime/hle -I runtime/gpu -I runtime/ui -I runtime/audio -I runtime/mods -I . \
  $EXTRA_INC $PKGCFLAGS"
LIBS="$LIBPATHS $PKGLIBS -lz -lpthread -ldl -lm"

OD=gen/linux/obj; mkdir -p $OD

# the Vulkan shaders first (gpu_vulkan.cpp includes gen/gpu_spv.h)
if ! command -v glslc >/dev/null 2>&1 && [ ! -f gen/gpu_spv.h ]; then
  echo "build_linux.sh: glslc not found and gen/gpu_spv.h missing (install the shader compiler, or copy the header from another build)" 1>&2; exit 1
fi
if command -v glslc >/dev/null 2>&1; then python3 tools/build/spv2h.py || exit 1; fi
GAMEID=ULES-00600
APPVER=${APPVER:-$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' CHANGELOG.md | head -1)}
STAMP="${OPT:--O2} $GAMEID $APPVER linux"
[ "$(cat $OD/flags.txt 2>/dev/null)" = "$STAMP" ] || { rm -f $OD/*.o $OD/*.d; echo "$STAMP" > $OD/flags.txt; }
export CXXOPT="${OPT:--O2}" GAMEID APPVER

# every runtime and decomp source, except the Direct3D 11 backend; the shim is added
# (runtime/*/*.cpp already covers runtime/plat/: sort -u keeps one of each)
CPPSRCS=$(ls runtime/plat/*.cpp runtime/*/*.cpp decomp/decomp.cpp decomp/*/*.cpp | grep -v 'gpu_d3d11.cpp' | sort -u)

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
  # one "[n/total] source" line per file: the progress the launcher's Build page reads
  TOTAL=$(printf '%s\n' $TODO | wc -l | tr -d ' ')
  i=0; LIST=""
  for p in $TODO; do i=$((i+1)); LIST="$LIST $i ${p%%:*} ${p#*:}"; done
  echo "$LIST" | tr ' ' '\n' | grep . | xargs -P ${JOBS:-$(nproc)} -n3 sh -c '
    n=$0; s=$1; o=$2
    echo "[$n/'"$TOTAL"'] $s"
    g++ '"$CXXFLAGS_COMMON"' "-DTF_GAME_ID=\"$GAMEID\"" "-DTF_APP_VERSION=\"${APPVER:-0.0.0}\"" -MMD -c "$s" -o "$o" 2> "${o%.o}.err" || { cat "${o%.o}.err"; rm -f "${o%.o}.err"; exit 1; }
    [ -s "${o%.o}.err" ] && cat "${o%.o}.err" 1>&2
    rm -f "${o%.o}.err"
  '
fi

EXE="${OUT:-gen/linux/TagForceRecomp}"
mkdir -p "$(dirname "$EXE")"
g++ -o "$EXE" $OD/*.o $LIBS -Wl,-rpath,'$ORIGIN' $RPATHS

if [ -z "$OUT" ]; then
  # the repository root is also a ready-to-run install, like the Windows build (tools/build.sh):
  # the game, TinyCC and the one header the generated code includes, beside each other
  # (tcc/, include/ and ./TagForceRecomp are git-ignored; jogo/ and gen/ come from your ISO)
  mkdir -p tcc include
  cp -ru gen/linux/tcc/. tcc/
  cp -u gen/linux/include/psp_rt.h include/
  cp "$EXE" ./TagForceRecomp 2>/dev/null || echo "note: ./TagForceRecomp is in use; the root copy was not updated"
fi
echo "built: $EXE"
