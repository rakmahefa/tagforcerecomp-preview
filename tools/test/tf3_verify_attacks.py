"""Compare TF3 attack-target protection with real TF1 donors in controlled engine fixtures."""
import json
import os
import sys
import tf
from tf3_probe_client import Probe, field


def main():
    os.environ['TF3_TEST_PROBE'] = '1'
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards')
    rows = [x.split(' ', 3) for x in open(os.path.join(pack, 'ported.txt'), encoding='utf-8') if x.strip()]
    gids = {int(x[1]): int(x[0]) for x in rows}
    hunter, freya = gids[7038], gids[7049]
    # Each tuple names a defender, optional companion, expected legality, and protection-check mode.
    definitions = [
        ('Command Knight alone', 5017, None, 1, 1),
        ('Command Knight protected', 5017, 4924, 0, 1),
        ('Command Knight resolution', 5017, 4924, 1, 0),
        ('Bone Tower alone', 5956, None, 1, 1),
        ('Bone Tower protected', 5956, 'Skull Servant', 0, 1),
        ('Hunter alone', hunter, None, 1, 1),
        ('Hunter with WIND', hunter, 'Luster Dragon', 0, 1),
        ('Hunter with EARTH', hunter, 4009, 1, 1),
        ('Hunter with another Hunter', hunter, hunter, 0, 1),
        ('Freya alone', freya, None, 1, 1),
        ('Freya with Fairy', freya, 4924, 0, 1),
        ('Freya with non-Fairy', freya, 4009, 1, 1),
        ('Freya with another Freya', freya, freya, 1, 1),
    ]
    # Resolve names from locally extracted properties rather than relying on guessed donor IDs.
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    sys.path.insert(0, os.path.join(root, 'tools', 'game'))
    import cartas
    by_name = {c.nome: c.id for c in cartas.CARTAS.values()}
    definitions = [(name, defender, by_name[companion] if isinstance(companion, str) else companion, expected, mode)
                   for name, defender, companion, expected, mode in definitions]
    results = []
    tf.duelo(['--lento'])
    try:
        probe = Probe()
        for name, defender, companion, expected, mode in definitions:
            fixtures = [field(0, 0, 4009), field(1, 0, defender)]
            if companion is not None:
                fixtures.append(field(1, 1, companion))
            actual = probe.attack(fixtures, 1, mode)
            result = dict(case=name, expected=expected, actual=actual, passed=actual == expected)
            results.append(result)
            print('PASS' if result['passed'] else 'FAIL', name, actual, 'expected', expected, flush=True)
            if expected == 0 and mode == 1:
                fixtures[1][4] = 0
                actual = probe.attack(fixtures, 1)
                results.append(dict(case=name + ' negated', expected=1, actual=actual, passed=actual == 1))
                print('PASS' if actual == 1 else 'FAIL', name, 'negated:', actual, flush=True)
                if defender in (hunter, freya):
                    fixtures[1][4] = 1
                    variations = []
                    face_down = [f[:] for f in fixtures]; face_down[2][6] |= 2
                    variations.append(('face-down companion', face_down, 1, 1))
                    companion_negated = [f[:] for f in fixtures]; companion_negated[2][4] = 0
                    variations.append(('negated companion still counts', companion_negated, 0, 1))
                    other_side = [f[:] for f in fixtures]; other_side[2][0] = 0
                    variations.append(('companion on other side', other_side, 1, 1))
                    variations.append(('companion removed', fixtures[:2], 1, 1))
                    variations.append(('established attack', fixtures, 1, 0))
                    for suffix, fs, wanted, protection in variations:
                        actual = probe.attack(fs, 1, protection)
                        results.append(dict(case=name + ' ' + suffix, expected=wanted, actual=actual, passed=actual == wanted))
                        print('PASS' if actual == wanted else 'FAIL', name, suffix, actual, 'expected', wanted, flush=True)
        for name, cid, expected in [('normal control target', 4009, 1), ('Blindly Loyal Goblin', 5714, 0),
                                    ('Mataza the Zapper', 5898, 0), ('Horus LV4', 6098, 0), ('Botanical Lion', gids[7143], 0)]:
            fixtures = [field(1, 0, cid)]
            actual = probe.control(fixtures, 1)
            results.append(dict(case=name + ' control', expected=expected, actual=actual, passed=actual == expected))
            print('PASS' if actual == expected else 'FAIL', name, 'control:', actual, 'expected', expected, flush=True)
            fixtures[0][4] = 0
            actual = probe.control(fixtures, 1)
            results.append(dict(case=name + ' control negated', expected=1, actual=actual, passed=actual == 1))
            print('PASS' if actual == 1 else 'FAIL', name, 'control negated:', actual, flush=True)
        with open(os.path.join(tf.CTL, 'attack_results.json'), 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=1)
    finally:
        tf.stop()
    return 0 if all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    sys.exit(main())
