"""Activate Raigeki against Horus and an ordinary monster, with or without Venus."""
import argparse
import json
import os
import struct
import tf
from tf3_verify_sabre import read
from tf3_verify_venus_natural import place_from_hand


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['venus', 'baseline'], required=True)
    case = parser.parse_args().case
    os.environ.update(TF3_TEST_VENUS='1', TF3_TEST_VENUS_CHAIN='1', TF3_FORCE_COPIES='3',
                      TF3_FORCE_CARDS='4343', TF_PRIMEIRO='0', TF_DECK_J1='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        if case == 'venus':
            place_from_hand(0, 1, 0, venus)
        place_from_hand(1, 0, 0, 6099)
        place_from_hand(1, 0, 1, 5017)
        count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        steps = count if case == 'venus' else count - 1
        tf.run(' '.join(['LEFT w30'] * steps) + ' X w60 X w1600 shot:resolved')
        enemy = 0x09f79cec + 0xfac + 0x30
        horus = struct.unpack('<I', read(enemy, 4))[0] & 0x1fff
        warrior = struct.unpack('<I', read(enemy + 20, 4))[0] & 0x1fff
        passed = horus == 6099 and warrior == 0
        print(json.dumps(dict(case=case, horus=horus, warrior=warrior, passed=passed)), flush=True)
        return 0 if passed else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
