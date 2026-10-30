"""Check passive ATK/DEF rules of many TF3 cards in ONE duel session (about a minute for dozens of cases).

  python tools/test/tf3_stat_batch.py --mods gen/tf3sum --specs stats.json --cases cases.json [--exe gen/pkg/TagForceRecomp/TagForceRecomp_bg.exe]

specs  : stat rules in the format of tools/mods/tf3_stats.json, installed for this run only (stat.bin of a temporary mods copy)
cases  : [{"card": 7836, "side": 0, "fixtures": [{"side": 0, "slot": 0, "card": 7836 | "Dark Magician" | {filter}}, ...],
           "grave": {"0": ["Dark Magician", ...], "1": [...]}, "expect": [atk_delta, def_delta]}, ...]
         fixtures[0] is the monster asked about; the rule's source is the TF3 card `card` (a fixture that names it, the first such one).
         The delta is measured against the same board with the source disabled (TF3TestProbe fixtures, see tf3_probe_client.py).
Needs the test-only TF3TestProbe mod in --mods (see docs/internal/FERRAMENTAS.md).
"""
import json
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'mods'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'test'))
JOG, PASSO = 0x09f79cec, 0xfac


def main():
    a = sys.argv[1:]
    def opt(n, d=None):
        return a[a.index(n) + 1] if n in a else d
    if '--inner' not in a:
        # outer: build the isolated package + temporary mods, then re-run ourselves inside it with the right environment
        import tf3_efeitos_param as P
        mods = os.path.abspath(opt('--mods', 'gen/tf3sum'))
        tag = opt('--tag', 'statb')
        work = os.path.join(ROOT, 'gen', 'statbatch_' + tag)
        shutil.rmtree(work, ignore_errors=True)
        exe = os.path.abspath(opt('--exe', os.path.join(ROOT, 'gen', 'pkg', 'TagForceRecomp', 'TagForceRecomp_bg.exe')))
        pkg = os.path.join(work, 'pkg')
        shutil.copytree(os.path.dirname(exe), pkg, ignore=shutil.ignore_patterns('jogo', 'ms0', 'logs', 'texcache', '__pycache__'))
        m2 = os.path.join(work, 'mods')
        shutil.copytree(mods, m2)
        for d in os.listdir(m2):
            if d == 'TF3Cards' and os.path.exists(os.path.join(m2, d, 'disabled')):
                os.remove(os.path.join(m2, d, 'disabled'))
        rows = [x.split(' ', 3) for x in open(os.path.join(m2, 'TF3Cards', 'ported.txt'), encoding='utf-8') if x.strip()]
        gids = {int(x[1]): int(x[0]) for x in rows}
        specs = json.load(open(opt('--specs'), encoding='utf-8'))
        base = P._carregar(P.SPEC_STATS_AUTO, P.SPEC_STATS)
        base.update(specs)
        open(os.path.join(m2, 'TF3Cards', 'stat.bin'), 'wb').write(P.resolver_stats(gids, base))
        env = dict(os.environ, TF_TAG=tag, TF_SAVE_FROM=os.path.join(ROOT, 'ms0'), TF_MODS_FROM=m2, TF_RES='1',
                   TF_TEST_DATA=os.path.join(os.path.dirname(exe), 'jogo'), TF_TEST_EXE=os.path.join(pkg, os.path.basename(exe)))
        r = subprocess.run([sys.executable, os.path.abspath(__file__), '--inner', '--cases', os.path.abspath(opt('--cases')), '--mods', m2],
                           cwd=ROOT, env=env)
        return r.returncode
    import tf
    import tf3_efeitos_param as P
    from tf3_probe_client import Probe, field
    os.environ['TF3_TEST_PROBE'] = '1'
    rows = [x.split(' ', 3) for x in open(os.path.join(opt('--mods'), 'TF3Cards', 'ported.txt'), encoding='utf-8') if x.strip()]
    gids = {int(x[1]): int(x[0]) for x in rows}
    prop = P.propriedades(gids)

    def ref(c):
        if isinstance(c, int):
            return gids.get(c, c)
        if isinstance(c, str):
            return next(i for i, p in sorted(prop.items()) if p['name'].lower() == c.lower())
        return next(i for i, p in sorted(prop.items()) if P.combina(p, c))

    cases = json.load(open(opt('--cases'), encoding='utf-8'))
    tf.duelo(['--lento'])
    bad = 0
    try:
        probe = Probe()
        for n, c in enumerate(cases):
            fx, src = [], None
            for f in c['fixtures']:
                is_src = isinstance(f['card'], int) and f['card'] == c['card'] and src is None
                if is_src:
                    src = len(fx)
                fx.append(field(f['side'], f['slot'], ref(f['card']), f.get('link', 0)))
            # Graveyard seeding of both players
            for s in (0, 1):
                ids = [ref(x) for x in c.get('grave', {}).get(str(s), [])]
                base = JOG + s * PASSO
                tf.send('poke %08x %s' % (base + 20, struct.pack('<I', len(ids)).hex()))
                for k, i in enumerate(ids):
                    tf.send('poke %08x %s' % (base + 0x710 + 4 * k, struct.pack('<I', i).hex()))
            off = [x[:] for x in fx]
            if src is not None:
                off[src][4] = 0
            b = probe.stats(off, c['side'])
            on = probe.stats(fx, c['side'])
            delta = [on[0] - b[0], on[1] - b[1]]
            ok = delta == c['expect']
            bad += not ok
            print(('PASS' if ok else 'FAIL'), c['card'], c.get('label', ''), 'delta', delta, 'expected', c['expect'], flush=True)
    finally:
        tf.stop()
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
