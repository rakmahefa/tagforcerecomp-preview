"""Exercise Venus against the native activation-negation handler in isolated duels."""
import argparse
import json
import os
import struct
import tf
from tf3_probe_client import Probe, field


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', required=True, choices=['spell', 'trap', 'other', 'opposing', 'disabled', 'facedown', 'absent', 'side1'])
    case = parser.parse_args().case
    os.environ.update(TF3_TEST_PROBE='1', TF3_TEST_ACTIONS='1', TF3_TEST_VENUS='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    side = 1 if case == 'side1' else 0
    target = 1 - side if case == 'opposing' else side
    card = 4863 if case == 'trap' else 4009 if case == 'other' else 4909
    fixtures = [] if case == 'absent' else [field(side, 0, venus, enabled=0 if case == 'disabled' else 1,
                                                  flags=0x82 if case == 'facedown' else 0x80)]
    try:
        tf.duelo([])
        probe = Probe()
        flags = struct.unpack_from('<I', probe.query(fixtures, target, operation=11, attacker=(card, 0)))[0]
        expected = 0 if case in ('spell', 'trap', 'side1') else 14
        result = dict(case=case, flags=flags, expected=expected, passed=flags == expected)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
