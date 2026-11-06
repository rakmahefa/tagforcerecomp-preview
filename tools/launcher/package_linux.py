#!/usr/bin/env python3
"""Assembles the Linux release: a launcher and its payload, like the Windows
zip, but as a tar.gz with the DQ8Recomp-style layout:

    TagForceRecomp/
      tagforce-launcher   the launcher (Your disc, Tools, Options, Build, Play)
      libSDL2-2.0.so.0    SDL2, bundled (zlib license); the system one is used if present
      payload/
        source/           this repository, tracked files only (git archive)
        payload.json      version and what the payload holds
        README.md         what the payload is, and what is not in it yet
      LICENSE, THIRD_PARTY.md, README.md, README-LINUX.txt

    python3 tools/launcher/package_linux.py [--out gen/linux] [--version 0.6.0] [--no-build]

The payload carries NO game data (checked) and no game code: the launcher
copies the files from the player's own ISO, like the Windows release. Needs:
g++, SDL2, git (build.sh fetches Dear ImGui once); the tree must be committed,
because the payload's source is `git archive HEAD`.
"""
import argparse
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GAME_DATA = ('.iso', '.prx', '.pbp', '.at3', '.pmf')  # never in a package


def run(cmd):
    print('>', ' '.join(cmd), flush=True)
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        sys.exit('failed: %s' % cmd)


def sdl2_library():
    """The SDL2 the launcher links, as the loader will find it."""
    out = subprocess.run(['ldd', 'build/tagforce-launcher'], cwd=ROOT, capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = line.split('=>')
        if len(parts) == 2 and 'libSDL2-2.0.so.0' in parts[0]:
            path = parts[1].split()[0]
            if os.path.isfile(path):
                return path
    for pattern in ('/usr/lib/*/libSDL2-2.0.so.0*', '/usr/lib/*/SDL2/libSDL2-2.0.so.0*'):
        found = sorted(glob.glob(pattern))
        if found:
            return found[0]
    sys.exit('libSDL2-2.0.so.0 not found: install libsdl2-2.0-0 (or libsdl2-dev) first')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', default=os.path.join(ROOT, 'gen', 'linux'))
    parser.add_argument('--version', default=None)
    parser.add_argument('--no-build', action='store_true')
    args = parser.parse_args()
    version = args.version or subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=ROOT,
                                             capture_output=True, text=True).stdout.strip() or 'preview'
    out = os.path.abspath(args.out)
    dist = os.path.join(out, 'dist')
    pkg = os.path.join(dist, 'TagForceRecomp')
    payload = os.path.join(pkg, 'payload')
    source = os.path.join(payload, 'source')
    shutil.rmtree(dist, ignore_errors=True)
    os.makedirs(source)

    # 1. the launcher
    if not args.no_build:
        run(['sh', 'tools/launcher/build.sh'])
    elif not os.path.isfile(os.path.join(ROOT, 'build', 'tagforce-launcher')):
        sys.exit('--no-build needs build/tagforce-launcher; build once without it')
    shutil.copy(os.path.join(ROOT, 'build', 'tagforce-launcher'), pkg)
    os.chmod(os.path.join(pkg, 'tagforce-launcher'), 0o755)

    # 2. SDL2 beside it ($ORIGIN rpath): the launcher runs even without a system SDL2
    shutil.copy(sdl2_library(), os.path.join(pkg, 'libSDL2-2.0.so.0'))

    # 3. the payload: this repository, tracked files only
    archive = subprocess.Popen(['git', 'archive', 'HEAD'], cwd=ROOT, stdout=subprocess.PIPE)
    extract = subprocess.Popen(['tar', '-x', '-C', source], stdin=archive.stdout)
    archive.stdout.close()
    if extract.wait() or archive.wait():
        sys.exit('git archive HEAD failed; commit the tree first')

    # 4. the payload's manifest and its README
    with open(os.path.join(payload, 'payload.json'), 'w', encoding='utf-8') as f:
        json.dump({
            'version': version,
            'game': 'ULES-00600',
            'platform': 'linux-x86_64',
            'launcher': 'tagforce-launcher',
            'source': 'source/',
            'fonts': 'source/tools/launcher/fonts/',
        }, f, indent=2)
        f.write('\n')
    with open(os.path.join(payload, 'README.md'), 'w', encoding='utf-8') as f:
        f.write(
            '# The payload\n'
            '\n'
            'Everything the Linux launcher works from, in one download:\n'
            '\n'
            '- `source/` - this repository (code, tools, documentation), as it was released;\n'
            '- `payload.json` - the release this folder belongs to.\n'
            '\n'
            'It carries **no game data and no game code**: the launcher copies the files from your\n'
            'own ISO (European release, ULES-00600) and translates the game on your machine.\n'
            'In this preview the Windows runtime does not build on Linux yet, so Build prepares\n'
            'the game files and the translated code, and Play starts the game as soon as a Linux\n'
            'build of the runtime lands beside the launcher. The C++ compiler and the other tools\n'
            'come from your distribution (the Tools page says which).\n')

    # 5. documents, and the three-step start for Linux
    for name in ('LICENSE', 'THIRD_PARTY.md', 'README.md'):
        shutil.copy(os.path.join(ROOT, name), pkg)
    with open(os.path.join(pkg, 'README-LINUX.txt'), 'w', encoding='utf-8') as f:
        f.write(
            'Tag Force Recomp - Linux preview %s\n'
            '\n'
            '1. Open a terminal in this folder and run:      ./tagforce-launcher\n'
            '2. Drag your disc image onto the window (Yu-Gi-Oh! GX Tag Force, European release\n'
            '   ULES-00600 - your own copy), then walk the pages with Continue.\n'
            '3. Press Start building; see README-LINUX above and docs/LINUX.md for what this\n'
            '   preview does and does not do on Linux yet.\n'
            '\n'
            'Nothing from the game is included here; everything is made on your computer from\n'
            'your own disc. If the launcher does not start, install SDL2 (sudo apt install\n'
            'libsdl2-2.0-0) - libSDL2-2.0.so.0 is bundled beside it, but a graphics driver\n'
            '(Mesa) is needed, as on any Linux desktop.\n' % version)

    # 6. checks: no game data, and the layout the launcher expects
    bad = [p for p in glob.glob(os.path.join(pkg, '**', '*'), recursive=True)
           if os.path.isfile(p) and p.lower().endswith(GAME_DATA)]
    if bad:
        sys.exit('suspicious files in the package: %s' % bad)
    for needed in ('tagforce-launcher', 'libSDL2-2.0.so.0', 'payload/payload.json',
                   'payload/source/tools/launcher/build.sh'):
        if not os.path.isfile(os.path.join(pkg, needed)):
            sys.exit('missing from the package: %s' % needed)

    # 7. the tar.gz
    archive_path = os.path.join(out, 'TagForceRecomp-%s-Linux-x86_64.tar.gz' % version)
    subprocess.run(['tar', '-C', dist, '-czf', archive_path, 'TagForceRecomp'], check=True)
    digest = hashlib.sha256(open(archive_path, 'rb').read()).hexdigest()
    print('package: %s (%.1f MB)' % (archive_path, os.path.getsize(archive_path) / 1048576))
    print('sha256: %s' % digest)


if __name__ == '__main__':
    main()
