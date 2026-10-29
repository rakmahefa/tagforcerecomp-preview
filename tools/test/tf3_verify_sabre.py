"""Mill a controlled Deck instance and resolve Sabre's native Graveyard equip routine."""
import argparse
import json
import os
import struct
import tf
from tf3_probe_client import Probe


def read(address, size):
    return bytes.fromhex(tf.send('peek %08x %d' % (address, size)).split()[-1])


def write(address, data):
    tf.send('poke %08x %s' % (address, data.hex()))


def state():
    raw = read(0x09f79cec, 2048)
    return dict(deck=struct.unpack_from('<I', raw, 16)[0],
                grave=[struct.unpack_from('<I', raw, 0x710 + i * 4)[0] & 0x1fff
                       for i in range(struct.unpack_from('<I', raw, 20)[0])],
                field=[list(struct.unpack_from('<5I', raw, 0x30 + i * 20)) for i in range(11)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['valid', 'wrong', 'opponent', 'decline', 'double', 'macro', 'fissure'], default='valid')
    args = parser.parse_args()
    monster_id = 4009 if args.case == 'wrong' else 7715
    os.environ.update(TF3_TEST_PROBE='1', TF3_TEST_ACTIONS='1', TF3_DEBUG_FX='1',
                      TF3_FORCE_COPIES='3', TF3_FORCE_CARDS='4009' if args.case == 'wrong' else '7591',
                      TF_PRIMEIRO='0', TF_DECK_J1='1')
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        hand = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        tf.run(' '.join(['LEFT w30'] * (hand - 1)) + ' X w60 X w200 shot:monster')
        slot = next(i for i, e in enumerate(state()['field'][:5]) if e[0] & 0x1fff == monster_id)
        probe = Probe()
        baseline = probe.current_stats(0, slot)
        if args.case in ('macro', 'fissure'):
            # Install the native continuous card into an isolated field fixture.
            base = 0x09f79cec
            hand_count = struct.unpack('<I', read(base + 12, 4))[0]
            hand = read(base + 0x120, hand_count * 4)
            card = struct.unpack_from('<I', hand)[0]
            card = (card & ~0x1fff) | (6682 if args.case == 'macro' else 6707)
            fixture_token = (((card >> 16) & 0x3fff) >> 6) * 2 + ((card >> 13) & 1)
            write(base + 0x30 + 5 * 20, struct.pack('<5I', card, 1, 1, 0, 0x80))
            write(base + 0x120, hand[4:] + bytes(4))
            write(base + 12, struct.pack('<I', hand_count - 1))
            old = struct.unpack('<I', read(0x09f7bc58 + fixture_token * 4, 4))[0]
            write(0x09f7bc58 + fixture_token * 4, struct.pack('<I', (old & ~0x1fff) | (card & 0x1fff)))
        if args.case == 'opponent':
            probe.remove_current(0, slot, destroyed=False)
            opponent = 0x09f79cec + 0xfac
            hand_count = struct.unpack('<I', read(opponent + 12, 4))[0]
            hand = read(opponent + 0x120, hand_count * 4)
            card = struct.unpack_from('<I', hand)[0]
            card = (card & ~0x1fff) | monster_id
            owner_token = ((((card >> 16) & 0x3fff) >> 6) * 2 + ((card >> 13) & 1))
            write(opponent + 0x30 + slot * 20, struct.pack('<5I', card, 1, 1, 0, 0x80))
            write(opponent + 0x120, hand[4:] + bytes(4))
            write(opponent + 12, struct.pack('<I', hand_count - 1))
            old = struct.unpack('<I', read(0x09f7bc58 + owner_token * 4, 4))[0]
            write(0x09f7bc58 + owner_token * 4, struct.pack('<I', (old & ~0x1fff) | monster_id))
        # Controlled Deck fixture preserves the original instance token and owner.
        deck = 0x09f79cec + 0x3a0
        count = 2 if args.case == 'double' else 1
        tokens = []
        for i in range(count):
            original = struct.unpack('<I', read(deck + i * 4, 4))[0]
            token = (((original >> 16) & 0x3fff) >> 6) * 2 + ((original >> 13) & 1)
            tokens.append(token)
            write(deck + i * 4, struct.pack('<I', (original & ~0x1fff) | 7860))
            instance = struct.unpack('<I', read(0x09f7bc58 + token * 4, 4))[0]
            write(0x09f7bc58 + token * 4, struct.pack('<I', (instance & ~0x1fff) | 7860))
        before = state()
        probe.mill_current(0, count)
        tf.run('w300 shot:milled O w60 shot:activation')
        if args.case == 'decline':
            tf.run('RIGHT w40 X w300 shot:declined')
        else:
            tf.run('X w300 shot:target X w120 X w300 shot:equipped')
            if args.case == 'double':
                tf.run('X w300 shot:second_prompt w300 X w180 X w300 shot:second_animation w600 shot:second_equipped')
        after = state()
        stats = probe.current_stats(1 if args.case == 'opponent' else 0, slot)
        equipped = [i for i, e in enumerate(after['field'][5:10], 5) if e[0] & 0x1fff == 7860]
        expected = count if args.case in ('valid', 'double', 'fissure') else 0
        result = dict(case=args.case, tokens=tokens, before=before, after=after,
                      baseline=baseline, stats=stats, equipped=equipped)
        result['passed'] = (after['deck'] == before['deck'] - count and len(equipped) == expected and
                            after['grave'].count(7860) == (0 if args.case == 'macro' else count - expected) and
                            tuple(a-b for a, b in zip(stats, baseline)) == (700 * expected, 0, 0))
        with open(os.path.join(tf.CTL, 'sabre_results.json'), 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
