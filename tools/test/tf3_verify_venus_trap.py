"""Set a Trap, advance a turn and test Cyber Phoenix against its real activation."""
import argparse
import json
import os
import struct
import tf
from tf3_verify_sabre import read
from tf3_verify_venus_natural import place_from_hand


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['protected', 'unprotected'], required=True)
    case = parser.parse_args().case
    os.environ.update(TF3_TEST_VENUS='1', TF3_TEST_VENUS_CHAIN='1', TF3_FORCE_COPIES='3',
                      TF3_FORCE_CARDS='5914', TF_PRIMEIRO='0', TF_DECK_J1='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        tf.run(' '.join(['LEFT w30'] * (count - 1)) + ' X w60 X w300 shot:trap_set')
        tf.run('O w60 RIGHT w20 RIGHT w20 RIGHT w20 X w2400 shot:turn_three')
        if case == 'protected':
            place_from_hand(0, 1, 0, venus)
        place_from_hand(1, 0, 0, 6390)
        place_from_hand(1, 0, 1, 6663)
        tf.run('UP w60 shot:select X w60 shot:menu X w300 shot:prompt X w180 shot:target X w1500 shot:resolved')
        target = struct.unpack('<I', read(0x09f79cec + 0xfac + 0x30, 4))[0] & 0x1fff
        count = struct.unpack('<I', read(0x09f79cec + 20, 4))[0]
        grave = struct.unpack('<' + 'I' * count, read(0x09f79cec + 0x710, count * 4)) if count else ()
        resolved = any((card & 0x1fff) == 5914 for card in grave)
        passed = resolved and ((target == 0) if case == 'protected' else (target == 6390))
        print(json.dumps(dict(case=case, target=target, resolved=resolved, passed=passed)), flush=True)
        return 0 if passed else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
