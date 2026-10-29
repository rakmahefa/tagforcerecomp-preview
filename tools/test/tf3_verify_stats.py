"""Check implemented TF3 stat rules through the real duel engine in an isolated TF_CTL session.

Requires the test-only TF3TestProbe plugin and TF_TEST_EXE / TF_MODS_FROM / TF_TAG.
Fixtures temporarily replace field records during one query and are restored by the probe.
This validates stat behavior, not every clause of a card's effect.
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'mods'))
import tf3_efeitos_param as P
import tf
from tf3_probe_client import Probe, field


def main():
    os.environ['TF3_TEST_PROBE'] = '1'
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards')
    rows = [x.split(' ', 3) for x in open(os.path.join(pack, 'ported.txt'), encoding='utf-8') if x.strip()]
    gids = {int(x[1]): int(x[0]) for x in rows}
    prop = P.propriedades(gids)

    def pick(f):
        return next(i for i, p in sorted(prop.items()) if P.combina(p, f))

    def named(name):
        return pick({'name_is': name})

    specifications = P._carregar(P.SPEC_STATS_AUTO, P.SPEC_STATS)
    cases = []
    # These deltas are expectations from the card clauses, independent of the serializer.
    equip_deltas = {6842: (600, 0), 7000: (200, 200), 7125: (500, 0), 7152: (400, 0),
                    7309: (300, 0), 7390: (800, 0), 7446: (300, 0), 7691: (500, 0), 7749: (700, 0)}
    for cid, delta in equip_deltas.items():
        rule = specifications[str(cid)]
        target = pick(rule.get('target') or {'monster': True, 'normal': True})
        cases.append((cid, 'equip bonus', [field(0, 0, target), field(0, 5, gids[cid], 1)], 1, 0, delta))
        for source_side in (0, 1):
            for target_side in (0, 1):
                cases.append((cid, 'equip bonus', [field(target_side, 0, target),
                              field(source_side, 7, gids[cid], 1 if source_side == target_side else 6)],
                              1, target_side, delta))
        cases.append((cid, 'unattached equip', [field(0, 0, target), field(0, 5, gids[cid])], 1, 0, (0, 0)))
        if rule.get('target'):
            wrong = pick({'monster': True, 'normal': True, 'not': rule['target']})
            cases.append((cid, 'wrong equip target', [field(0, 0, wrong), field(0, 5, gids[cid], 1)], 1, 0, (0, 0)))

    def passive(cid, target, delta, others=(), target_side=0, source_slot=1):
        fs = [field(target_side, 0, target)]
        if target_side == 0 and target == gids[cid]:
            source_index = 0
        else:
            fs.append(field(0, source_slot, gids[cid])); source_index = 1
        for i, monster in enumerate(others):
            fs.append(field(0, 2 + i, monster))
        cases.append((cid, 'passive bonus', fs, source_index, target_side, delta))

    passive(6927, gids[6927], (400, 0), [named('Elemental Hero Avian')])
    passive(6934, gids[6934], (2000, 0), [named('Green Gadget')])
    passive(6934, gids[6934], (0, 0))
    passive(7038, gids[7038], (1000, 0), [named('Luster Dragon')])
    passive(7049, named('Shining Angel'), (400, 400))
    passive(7143, gids[7143], (600, 0), [named('Mushroom Man')])
    passive(7347, named('Hitotsu-Me Giant'), (-500, -500))
    passive(7347, named('Shining Angel'), (0, 0))
    passive(7399, named('Hitotsu-Me Giant'), (-400, 0), target_side=1, source_slot=5)
    passive(7457, named('Hitotsu-Me Giant'), (-500, 400), source_slot=10)
    passive(7457, named('The Melting Red Shadow'), (0, 0), source_slot=10)
    passive(7534, gids[7534], (400, 0), [pick({'monster': True, 'name_has': 'Ally of Justice'})])
    passive(7534, gids[7534], (0, 0))
    passive(7539, pick({'monster': True, 'name_has': 'Saber'}), (400, 0))
    passive(7627, named('The Melting Red Shadow'), (1200, 0), source_slot=10)
    passive(7627, named('Hitotsu-Me Giant'), (0, 0), source_slot=10)
    passive(7895, named('Celtic Guardian'), (400, 0))
    cases.append((7125, 'Spell-immune equip target',
                  [field(0, 0, named('Horus the Black Flame Dragon LV6')), field(0, 5, gids[7125], 1)], 1, 0, (0, 0)))
    passive(7399, named('Horus the Black Flame Dragon LV6'), (0, 0), target_side=1, source_slot=5)
    cases.append((7399, 'current level after Synchro Boost',
                  [field(1, 0, named('Hitotsu-Me Giant')), field(0, 5, gids[7399]), field(1, 5, gids[7691], 1)], 1, 1, (-500, 0)))

    results = []
    tf.duelo(['--lento'])
    try:
        query = Probe().stats

        for cid, label, fixtures, src, side, expected in cases:
            expected = expected + (1 if cid == 7691 and label == 'equip bonus' else 0,)
            disabled = [x[:] for x in fixtures]; disabled[src][4] = 0
            baseline = query(disabled, side)
            active = query(fixtures, side)
            delta = tuple(a - b for a, b in zip(active, baseline))
            negated_again = query(disabled, side)
            ok = delta == expected and negated_again == baseline
            result = dict(card=cid, case=label, baseline=baseline, active=active, expected=expected, delta=delta, passed=ok)
            if src != 0:
                removed = query([x for i, x in enumerate(fixtures) if i != src], side)
                result['removed'] = removed
                result['passed'] = ok and removed == baseline
            results.append(result)
            print(('PASS' if result['passed'] else 'FAIL'), cid, label, delta, 'expected', expected, flush=True)
        out = os.path.join(tf.CTL, 'stats_results.json')
        with open(out, 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=1)
        print('Results:', out, flush=True)
    finally:
        tf.stop()
    return 0 if all(x['passed'] for x in results) else 1


if __name__ == '__main__':
    sys.exit(main())
