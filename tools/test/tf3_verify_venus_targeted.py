"""Exercise a targeted Spell against Frontier Wiseman using controlled field fixtures."""
import argparse
import json
import os
import struct
import tf
from tf3_verify_sabre import read, write
from tf3_verify_venus_natural import place_from_hand


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['protected', 'unprotected', 'disabled', 'opposing'], required=True)
    parser.add_argument('--source', choices=['frontier', 'phoenix', 'rod', 'horus'], default='frontier')
    args = parser.parse_args()
    case = args.case
    os.environ.update(TF3_TEST_VENUS='1', TF3_TEST_VENUS_CHAIN='1', TF3_FORCE_COPIES='3',
                      TF3_FORCE_CARDS='5214', TF_PRIMEIRO='0', TF_DECK_J1='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        if case in ('protected', 'disabled'):
            place_from_hand(0, 1, 0, venus)
            if case == 'disabled':
                write(0x09f79cec + 0x30 + 8, struct.pack('<H', 0))
        monster, source = {'frontier': (5017, 5322), 'phoenix': (6390, 6663), 'rod': (5017, 5728), 'horus': (6099, 0)}[args.source]
        place_from_hand(1, 0, 0, monster)
        if source:
            place_from_hand(1, 0, 5 if args.source == 'rod' else 1, source)
        if case == 'opposing':
            place_from_hand(1, 0, 2, venus)
        if args.source == 'rod':
            # Native equipment node 1 attaches opponent Spell slot 5 to monster slot 0.
            write(0x09f7c460, struct.pack('<4H', 0x501, 10, 0, 0))
            write(0x09f79cec + 0xfac + 0x30 + 10, struct.pack('<H', 1))
        count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        steps = count if case in ('protected', 'disabled') else count - 1
        target_route = 'UP w60 ' + ('RIGHT w30 RIGHT w30 RIGHT w30 RIGHT w60' if case == 'protected' else 'DOWN w60')
        tf.run(' '.join(['LEFT w30'] * steps) + ' X w60 X w300 shot:prompt X w180 ' + target_route + ' shot:target X w1500 shot:resolved')
        target = struct.unpack('<I', read(0x09f79cec + 0xfac + 0x30, 4))[0] & 0x1fff
        count = struct.unpack('<I', read(0x09f79cec + 20, 4))[0]
        grave = struct.unpack('<' + 'I' * count, read(0x09f79cec + 0x710, count * 4)) if count else ()
        resolved = any((card & 0x1fff) == 5214 for card in grave)
        passed = resolved and ((target == 0) if case == 'protected' and args.source != 'horus' else (target == monster))
        print(json.dumps(dict(case=case, source=args.source, target=target, resolved=resolved, passed=passed)), flush=True)
        return 0 if passed else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
