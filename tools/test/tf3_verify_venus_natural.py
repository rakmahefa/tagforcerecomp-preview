"""Activate Pot of Greed against a controlled Imperial Order field fixture."""
import argparse
import json
import os
import struct
import tf
from tf3_verify_sabre import read, write


def place_from_hand(side, hand_index, slot, card_id):
    base = 0x09f79cec + 0xfac * side
    count = struct.unpack('<I', read(base + 12, 4))[0]
    hand = read(base + 0x120, count * 4)
    card = struct.unpack_from('<I', hand, hand_index * 4)[0]
    card = (card & ~0x1fff) | card_id
    token = (((card >> 16) & 0x3fff) >> 6) * 2 + ((card >> 13) & 1)
    write(base + 0x30 + slot * 20, struct.pack('<5I', card, 1, 1, 0, 0x80))
    write(base + 0x120, hand[:hand_index * 4] + hand[(hand_index + 1) * 4:] + bytes(4))
    write(base + 12, struct.pack('<I', count - 1))
    instance = struct.unpack('<I', read(0x09f7bc58 + token * 4, 4))[0]
    write(0x09f7bc58 + token * 4, struct.pack('<I', (instance & ~0x1fff) | card_id))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['protected', 'negated', 'plain', 'venus_only', 'canceller_venus', 'canceller_only', 'opposing', 'disabled', 'facedown', 'jammer_venus', 'jammer_only'], required=True)
    case = parser.parse_args().case
    os.environ.update(TF3_TEST_VENUS='1', TF3_FORCE_COPIES='3', TF3_FORCE_CARDS='4844', TF_PRIMEIRO='0', TF_DECK_J1='1')
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards', 'ported.txt')
    rows = [line.split(' ', 3) for line in open(pack, encoding='utf-8') if line.strip()]
    venus = next(int(row[0]) for row in rows if int(row[1]) == 7347)
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        if case in ('protected', 'venus_only', 'canceller_venus', 'disabled', 'facedown', 'jammer_venus'):
            place_from_hand(0, 1, 0, venus)
            if case == 'disabled':
                write(0x09f79cec + 0x30 + 8, struct.pack('<H', 0))
            if case == 'facedown':
                write(0x09f79cec + 0x30 + 16, struct.pack('<I', 0x82))
        if case == 'opposing':
            place_from_hand(1, 0, 1, venus)
        if case in ('protected', 'negated', 'opposing', 'disabled', 'facedown'):
            place_from_hand(1, 0, 5, 4960)
        if case in ('canceller_venus', 'canceller_only'):
            place_from_hand(1, 0, 0, 5594)
        if case.startswith('jammer_'):
            place_from_hand(1, 0, 5, 4862)
            write(0x09f79cec + 0xfac + 0x30 + 5 * 20 + 4, struct.pack('<I', 0))
            write(0x09f79cec + 0xfac + 0x30 + 5 * 20 + 8, struct.pack('<I', 0))
            write(0x09f79cec + 0xfac + 0x30 + 5 * 20 + 16, struct.pack('<I', 0x100000))
        count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        initial_hand = struct.unpack('<' + 'I' * count, read(0x09f79cec + 0x120, count * 4))
        initial_pots = sum((card & 0x1fff) == 4844 for card in initial_hand)
        before = struct.unpack('<I', read(0x09f79cfc, 4))[0]
        # Removing a card from the hand fixture leaves the UI cursor at its old index.
        steps = count if case in ('protected', 'venus_only', 'canceller_venus', 'disabled', 'facedown', 'jammer_venus') else count - 1
        tf.run(' '.join(['LEFT w30'] * steps) + ' X w60 X w1600 shot:resolved')
        after = struct.unpack('<I', read(0x09f79cfc, 4))[0]
        after_count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        hand = struct.unpack('<' + 'I' * after_count, read(0x09f79cec + 0x120, after_count * 4))
        pot_count = sum((card & 0x1fff) == 4844 for card in hand)
        field_bytes = read(0x09f79cec + 0x30, 11 * 20)
        field_pots = [(struct.unpack_from('<I', field_bytes, slot * 20)[0] & 0x1fff,
                       struct.unpack_from('<I', field_bytes, slot * 20 + 16)[0]) for slot in range(5, 10)]
        expected = 0 if case in ('negated', 'canceller_venus', 'canceller_only', 'opposing', 'disabled', 'facedown', 'jammer_only') else 2
        passed = before - after == expected
        if case.startswith('canceller_'):
            set_pots = sum(card == 4844 and (flags & 0x100000) != 0 for card, flags in field_pots)
            passed = passed and initial_pots > 0 and pot_count + set_pots == initial_pots
        if case.startswith('jammer_'):
            opp = 0x09f79cec + 0xfac
            grave_count = struct.unpack('<I', read(opp + 20, 4))[0]
            grave = struct.unpack('<' + 'I' * grave_count, read(opp + 0x710, grave_count * 4)) if grave_count else ()
            passed = passed and any((card & 0x1fff) == 4862 for card in grave)
        result = dict(case=case, before=before, after=after, expected=expected, pot_count=pot_count,
                      field_pots=field_pots, passed=passed)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
