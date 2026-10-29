"""Check attack-to-Defense behavior and lock expiry in an isolated natural duel.

The fixture adds ATK/DEF only to the test pack so the monster survives until
turn seven. --native compares Goblin Attack Force. Original test pack files
are restored after the copied-session duel, including on failure.
"""
import json
import os
import struct
import sys
import tf
from tf3_probe_client import Probe

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'mods'))
import tf3_efeitos_param as P


def field():
    data = bytes.fromhex(tf.send('peek 09f79d1c 100').split()[-1])
    return [list(struct.unpack_from('<5I', data, 20 * i)) for i in range(5)]


def main():
    source = os.path.realpath(os.environ['TF_MODS_FROM'])
    if os.path.commonpath([os.path.realpath(os.path.join(ROOT, 'gen')), source]) != os.path.realpath(os.path.join(ROOT, 'gen')):
        raise RuntimeError('Position fixtures require an isolated mod pack inside gen/')
    pack = os.path.join(source, 'TF3Cards')
    with open(os.path.join(pack, 'ported.txt'), encoding='utf-8') as f:
        gids = {int(r[1]): int(r[0]) for line in f if (r := line.split(' ', 3)) and len(r) == 4}
    original = {name: open(os.path.join(pack, name), 'rb').read() for name in ('stat.bin', 'trigger.bin')}
    native = '--native' in sys.argv
    card = 5145 if native else 7842
    slot = 5145 if native else gids[card]
    os.environ.update(TF3_TEST_PROBE='1', TF3_FORCE_CARDS=str(card), TF3_FORCE_COPIES='3', TF_PRIMEIRO='0', TF_DECK_J1='1')
    try:
        stats = P._carregar(P.SPEC_STATS_AUTO, P.SPEC_STATS)
        stats['7842'] = dict(scope='self', atk=1000, **{'def': 3000})
        stats['5145'] = dict(scope='self', **{'def': 3000})
        fixture_gids = dict(gids)
        fixture_gids[5145] = 5145
        with open(os.path.join(pack, 'stat.bin'), 'wb') as f: f.write(P.resolver_stats(fixture_gids, stats))
        with open(os.path.join(pack, 'trigger.bin'), 'wb') as f: f.write(P.resolver_gatilhos(gids))
        tf.duelo([])
        probe = Probe()
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        data = bytes.fromhex(tf.send('peek 09f79cec 512').split()[-1])
        count = struct.unpack_from('<I', data, 12)[0]
        hand = [struct.unpack_from('<H', data, 0x120 + i * 4)[0] & 0x1fff for i in range(count)]
        tf.run(' '.join(['LEFT w30'] * (count - 1 - hand.index(slot))) + ' X w60 X w300 shot:summoned')
        tf.run('O w60 RIGHT w20 RIGHT w20 RIGHT w20 X w2400 shot:turn_three')
        tf.run('O w60 RIGHT w20 X w200 X w60 X w60 X w600 shot:attacked')
        before = field()[2]
        tf.run('O w60 RIGHT w20 X w500 shot:battle_end')
        after = field()[2]
        permissions = [probe.current_position_permission(0, 2)]
        tf.run('O w60 RIGHT w20 RIGHT w20 RIGHT w20 X w2400 shot:next_turn')
        permissions.append(probe.current_position_permission(0, 2))
        tf.run('DOWN w40 DOWN w40 UP w40 UP w40 LEFT w40 LEFT w40 O w60 RIGHT w20 RIGHT w20 X w2400 X w100 X w2400 shot:following_turn')
        permissions.append(probe.current_position_permission(0, 2))
        final = field()[2]
        result = dict(card=card, before=before, after=after, final=final, permissions=permissions,
                      fixture_bonus={'ported_atk': 1000, 'def': 3000})
        result['passed'] = before[0] & 0x1fff == slot and after[0] & 0x1fff == slot and final[0] & 0x1fff == slot and before[1] >> 16 == 0 and after[1] >> 16 == 1 and permissions == [(0, 1), (0, 1), (1, 0)]
        with open(os.path.join(tf.CTL, 'position_results.json'), 'w', encoding='utf-8') as f: json.dump(result, f, indent=2)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()
        for name, data in original.items():
            with open(os.path.join(pack, name), 'wb') as f: f.write(data)


if __name__ == '__main__':
    sys.exit(main())
