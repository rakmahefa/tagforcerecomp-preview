"""Check equip target restrictions against native target selection in an isolated duel."""
import json
import os
import sys
import tf
from tf3_probe_client import Probe, field

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'mods'))
import tf3_efeitos_param as P


def main():
    os.environ['TF3_TEST_PROBE'] = '1'
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards')
    with open(os.path.join(pack, 'ported.txt'), encoding='utf-8') as f:
        gids = {int(r[1]): int(r[0]) for line in f if (r := line.split(' ', 3)) and len(r) == 4}
    props = P.propriedades(gids)
    specifications = P._carregar(P.SPEC_STATS_AUTO, P.SPEC_STATS)
    results = []
    try:
        tf.duelo([])
        probe = Probe()
        for cid in (6842, 7000, 7125, 7152, 7309, 7390, 7446, 7691, 7749):
            target_filter = specifications[str(cid)].get('target') or {'monster': True}
            valid = next(i for i, p in sorted(props.items()) if P.combina(p, target_filter))
            for side in (0, 1):
                result = probe.equip_target([field(side, 0, valid)], gids[cid], side)
                results.append(dict(card=cid, case='valid target', side=side, actual=result, passed=result != 0))
                result = probe.equip_target([field(side, 0, valid, enabled=0, flags=0)], gids[cid], side)
                results.append(dict(card=cid, case='face-down target', side=side, actual=result, passed=result == 0))
                if 'target' in specifications[str(cid)]:
                    wrong = next(i for i, p in sorted(props.items()) if P.combina(p, {'monster': True}) and not P.combina(p, target_filter))
                    result = probe.equip_target([field(side, 0, wrong)], gids[cid], side)
                    results.append(dict(card=cid, case='wrong target', side=side, actual=result, passed=result == 0))
        with open(os.path.join(tf.CTL, 'equip_results.json'), 'w', encoding='utf-8') as f:
            json.dump(results, f, indent=2)
        for r in results:
            print('PASS' if r['passed'] else 'FAIL', r, flush=True)
        return 0 if all(r['passed'] for r in results) else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    sys.exit(main())
