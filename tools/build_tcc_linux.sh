#!/bin/sh
# Builds TinyCC's shared library (libtcc.so, LGPL-2.1) from the official source into an output
# folder (default gen/linux/tcc): libtcc.so and TinyCC's own include/. The game's JIT loads
# libtcc.so beside itself at run time - it is never linked into the executable - and no
# distribution packages the shared library (Debian's tcc/libtcc-dev carry a static libtcc.a
# only), so the first tools/build_linux.sh run calls this once.
#
#   sh tools/build_tcc_linux.sh [out-dir]   Variables: TF_TCC_SRC=path (an existing source
#                                           tree to build instead of fetching), JOBS=n
#
# The source is the same official release the Windows zip ships (0.9.27), fetched once from
# https://download.savannah.gnu.org/releases/tinycc/ and kept under gen/ (git-ignored).
set -e
cd "$(dirname "$0")/.."
OUT="${1:-gen/linux/tcc}"
VER=0.9.27
SRC="${TF_TCC_SRC:-gen/tcc-$VER}"
TARBALL="gen/tcc-$VER.tar.bz2"
URL="https://download.savannah.gnu.org/releases/tinycc/tcc-$VER.tar.bz2"

if [ ! -f "$SRC/configure" ]; then
    if [ ! -f "$TARBALL" ]; then
        echo "TinyCC: fetching the $VER source ($URL)"
        mkdir -p gen
        if command -v curl >/dev/null 2>&1; then curl -fSL -o "$TARBALL" "$URL"
        elif command -v wget >/dev/null 2>&1; then wget -q -O "$TARBALL" "$URL"
        else echo "build_tcc_linux.sh: need curl or wget to fetch the TinyCC source (or point TF_TCC_SRC at an existing tree)" 1>&2; exit 1; fi
    fi
    rm -rf "$SRC" gen/.tcc-extract
    mkdir -p gen/.tcc-extract
    tar -xjf "$TARBALL" -C gen/.tcc-extract
    mv gen/.tcc-extract/* "$SRC"
    rmdir gen/.tcc-extract
fi

echo "TinyCC: building libtcc.so in $SRC (once)"
cd "$SRC"
[ -f Makefile ] || sh ./configure
make -j"${JOBS:-$(nproc)}" >gen_build.log 2>&1 || { tail -20 gen_build.log 1>&2; echo "build_tcc_linux.sh: the TinyCC build failed (see $SRC/gen_build.log)" 1>&2; exit 1; }

cd - >/dev/null
mkdir -p "$OUT"
cp -L "$SRC"/libtcc.so* "$OUT/"
[ -f "$SRC/libtcc1.a" ] && cp -u "$SRC/libtcc1.a" "$OUT/"
mkdir -p "$OUT/include"
cp -ru "$SRC/include/." "$OUT/include/"
echo "TinyCC: $OUT/libtcc.so ready"
