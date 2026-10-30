"""Print the game's FLIP classification (1 = FLIP monster) of card ids in one duel session.

  python tools/test/tf3_flip_kinds.py --mods gen/tf3sum 4507 5139 5651 ... [--exe gen/pkg/TagForceRecomp/TagForceRecomp_bg.exe]
Needs the test-only TF3TestProbe mod in --mods (see docs/internal/FERRAMENTAS.md).
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'test'))


def main():
    a = sys.argv[1:]
    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    ids = [int(x) for i, x in enumerate(a) if x.isdigit() and (i == 0 or a[i - 1] not in ('--mods', '--exe', '--tag'))]
    if '--inner' not in a:
        tag = opt('--tag', 'flipk')
        work = os.path.join(ROOT, 'gen', 'flipk_' + tag)
        shutil.rmtree(work, ignore_errors=True)
        exe = os.path.abspath(opt('--exe', os.path.join(ROOT, 'gen', 'pkg', 'TagForceRecomp', 'TagForceRecomp_bg.exe')))
        pkg = os.path.join(work, 'pkg')
        shutil.copytree(os.path.dirname(exe), pkg, ignore=shutil.ignore_patterns('jogo', 'ms0', 'logs', 'texcache', '__pycache__'))
        m2 = os.path.join(work, 'mods')
        shutil.copytree(os.path.abspath(opt('--mods', 'gen/tf3sum')), m2)
        env = dict(os.environ, TF_TAG=tag, TF_SAVE_FROM=os.path.join(ROOT, 'ms0'), TF_MODS_FROM=m2, TF_RES='1',
                   TF_TEST_DATA=os.path.join(os.path.dirname(exe), 'jogo'), TF_TEST_EXE=os.path.join(pkg, os.path.basename(exe)))
        return subprocess.run([sys.executable, os.path.abspath(__file__), '--inner', '--mods', m2] + [str(i) for i in ids], cwd=ROOT, env=env).returncode
    import tf
    from tf3_probe_client import Probe
    os.environ['TF3_TEST_PROBE'] = '1'
    tf.duelo(['--lento'])
    try:
        p = Probe()
        for i in ids:
            print('flip_kind', i, p.flip_kind(i), flush=True)
    finally:
        tf.stop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
