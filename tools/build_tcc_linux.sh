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
MIRROR="gen/tcc-$VER-mirror.tar.gz"
URL="https://download.savannah.gnu.org/releases/tinycc/tcc-$VER.tar.bz2"
URLM="https://github.com/TinyCC/tinycc/archive/refs/tags/release_0_9_27.tar.gz"

fetch() {   # fetch <url> <output>: curl or wget, whichever is installed
    if command -v curl >/dev/null 2>&1; then curl -fSL --retry 2 -o "$2" "$1"
    elif command -v wget >/dev/null 2>&1; then wget -q -O "$2" "$1"
    else echo "build_tcc_linux.sh: need curl or wget to fetch the TinyCC source (or point TF_TCC_SRC at an existing tree)" 1>&2; return 1; fi
}

if [ ! -f "$SRC/configure" ]; then
    if [ ! -f "$TARBALL" ] && [ ! -f "$MIRROR" ]; then
        echo "TinyCC: fetching the $VER source ($URL)"
        mkdir -p gen
        fetch "$URL" "$TARBALL" || {
            rm -f "$TARBALL"
            echo "TinyCC: savannah did not answer; trying the GitHub mirror of the same release"
            fetch "$URLM" "$MIRROR" || { rm -f "$MIRROR"; echo "build_tcc_linux.sh: could not fetch the TinyCC source" 1>&2; exit 1; }
        }
    fi
    rm -rf "$SRC" gen/.tcc-extract
    mkdir -p gen/.tcc-extract
    if [ -f "$TARBALL" ]; then tar -xjf "$TARBALL" -C gen/.tcc-extract; else tar -xzf "$MIRROR" -C gen/.tcc-extract; fi
    mv gen/.tcc-extract/* "$SRC"
    rmdir gen/.tcc-extract
    [ "$(cat "$SRC/VERSION" 2>/dev/null)" = "$VER" ] || { echo "build_tcc_linux.sh: the extracted source is not TinyCC $VER" 1>&2; exit 1; }
fi

echo "TinyCC: building libtcc.so in $SRC (once)"
cd "$SRC"
# not [ -f Makefile ]: the official tarball ships one (a 2017 file tracked in git) whose first
# rule is "config.mak: Please run ./configure." - configure is what writes config.mak, so that
# is the file to gate on (a tree configured by an earlier run keeps it; make alone never does)
[ -f config.mak ] || sh ./configure
# not a plain "make": that also builds libtcc1.a (a FORCE target), whose bcheck.c does not compile
# against glibc >= 2.34 - the archive is built separately below, without the bounds checker
make -j"${JOBS:-$(nproc)}" libtcc.so tcc >gen_build.log 2>&1 || { tail -20 gen_build.log 1>&2; echo "build_tcc_linux.sh: the TinyCC build failed (see $SRC/gen_build.log)" 1>&2; exit 1; }
# libtcc1.a: 0.9.27's bcheck.c uses __malloc_hook, which glibc >= 2.34 removed; the bounds checker
# (-b) is never used by the game, so build the archive without bcheck.o. Without this file NOTHING
# links: every module dies with "tcc: error: file '<dir>/libtcc1.a' not found" (0.9.27 looks for it
# in tcc_set_lib_path's dir, which is exactly this $OUT).
make -C lib ../libtcc1.a BCHECK_O= >gen_libtcc1.log 2>&1 || { tail -20 gen_libtcc1.log 1>&2; echo "build_tcc_linux.sh: libtcc1.a did not build (see $SRC/gen_libtcc1.log)" 1>&2; exit 1; }
[ -f libtcc1.a ] || { echo "build_tcc_linux.sh: libtcc1.a is missing after the build" 1>&2; exit 1; }

cd - >/dev/null
mkdir -p "$OUT"
cp -L "$SRC"/libtcc.so* "$OUT/"
cp -u "$SRC/libtcc1.a" "$OUT/"
mkdir -p "$OUT/include"
cp -ru "$SRC/include/." "$OUT/include/"
echo "TinyCC: $OUT/libtcc.so ready"
