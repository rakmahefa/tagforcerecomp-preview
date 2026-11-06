#!/bin/sh
# Builds the Linux launcher: tools/launcher -> build/tagforce-launcher.
#
#   sh tools/launcher/build.sh
#
# Needs: g++, SDL2 (headers + library), git (once: Dear ImGui is fetched,
# pinned to v1.91.9, into tools/launcher/deps/imgui, which git ignores).
# SDL2 is found with sdl2-config; override with SDL2_CFLAGS and SDL2_LIBS.
# The binary looks for libSDL2 beside itself first ($ORIGIN), then the system.
set -e
cd "$(dirname "$0")/../.."
SRC=tools/launcher
DEPS=$SRC/deps
IMGUI=$DEPS/imgui
OUT=${TF_LAUNCHER_OUT:-build/tagforce-launcher}
mkdir -p "$(dirname "$OUT")"

if [ ! -d "$IMGUI" ]; then
    echo "fetching Dear ImGui v1.91.9 (pinned) into $IMGUI"
    mkdir -p "$DEPS"
    git clone --depth 1 --branch v1.91.9 https://github.com/ocornut/imgui.git "$IMGUI"
fi

: "${SDL2_CFLAGS:=$(sdl2-config --cflags)}"
: "${SDL2_LIBS:=$(sdl2-config --libs)}"

# -std=gnu++17: the launcher uses POSIX process calls (fork, exec, pipes).
g++ -std=gnu++17 -O2 -Wall -Wno-misleading-indentation -Wno-unused-parameter \
    -I"$IMGUI" -I"$IMGUI/backends" $SDL2_CFLAGS \
    -o "$OUT" \
    $SRC/launcher_main.cpp $SRC/launcher_app.cpp $SRC/launcher_iso.cpp $SRC/launcher_tools.cpp \
    $SRC/launcher_pipeline.cpp $SRC/launcher_png.cpp \
    "$IMGUI/imgui.cpp" "$IMGUI/imgui_draw.cpp" "$IMGUI/imgui_tables.cpp" "$IMGUI/imgui_widgets.cpp" \
    "$IMGUI/backends/imgui_impl_sdl2.cpp" "$IMGUI/backends/imgui_impl_opengl3.cpp" \
    -Wl,-rpath,'$ORIGIN' $SDL2_LIBS -lpthread -ldl

echo "built: $OUT"
