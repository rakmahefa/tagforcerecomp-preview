"""Compare Venus protection with the engine's continuous effect-negation predicate."""
import argparse
import json
import os
import struct
import tf
from tf3_probe_client import Probe, field


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', required=True, choices=['own0', 'own1', 'opp0', 'opp1', 'disabled', 'facedown', 'absent', 'no_jinzo', 'jinzo_on', 'jinzo_off', 'decree_on', 'decree_off', 'canceller_on', 'canceller_off', 'order_on', 'order_off'])
    case = parser.parse_args().case
    os.environ.update(TF3_TEST_PROBE='1', TF3_TEST_VENUS='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    extended = case.endswith('_on') or case.endswith('_off')
    side = 1 if case.endswith('1') else 0
    venus_side = 1 - side if case.startswith('opp') else side
    fixtures = [] if case == 'no_jinzo' else [field(1, 0, 4758)]
    card = 4863
    if extended:
        source = case.rsplit('_', 1)[0]
        fixtures = [field(1, 5 if source in ('decree', 'order') else 0,
                          {'jinzo': 4758, 'decree': 4866, 'canceller': 5594, 'order': 4960}[source])]
        card = 4909 if source in ('canceller', 'order') else 4863
    if case != 'absent' and not case.endswith('_off'):
        fixtures.append(field(venus_side, 1, venus, enabled=0 if case == 'disabled' else 1,
                              flags=0x82 if case == 'facedown' else 0x80))
    try:
        tf.duelo([])
        actual = struct.unpack_from('<I', Probe().query(fixtures, side, operation=12, attacker=(card, 0)))[0]
        expected = (0 if case.endswith('_on') else 1) if extended else (0 if case in ('own0', 'own1', 'no_jinzo') else 1)
        result = dict(case=case, actual=actual, expected=expected, passed=actual == expected)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
