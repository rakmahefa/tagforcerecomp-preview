"""Resolve equip Graveyard effects through native removal actions in an isolated duel.

The monster/equip is played normally. The opt-in probe supplies a controlled
removal cause; the engine then processes its own pending events and draws/LP.
This is engine integration, not proof that the AI chooses the removal spell.
"""
import argparse
import json
import os
import re
import struct
import sys
import tf
from tf3_probe_client import Probe


def state():
    raw = bytes.fromhex(tf.send('peek 09f79cec 4016').split()[-1])
    return dict(lp=struct.unpack_from('<I', raw, 0)[0], opponent_lp=struct.unpack_from('<I', raw, 4012)[0],
                hand=struct.unpack_from('<I', raw, 12)[0], deck=struct.unpack_from('<I', raw, 16)[0],
                field=[list(struct.unpack_from('<5I', raw, 0x30 + i * 20)) for i in range(11)])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--card', choices=['axe', 'tank'], default='axe')
    parser.add_argument('--case', choices=['opponent', 'own', 'facedown', 'send', 'banish'], default='opponent')
    args = parser.parse_args()
    card = 7125 if args.card == 'axe' else 6842
    set_card = args.card == 'tank' or args.case == 'facedown'
    os.environ.update(TF3_TEST_PROBE='1', TF3_TEST_ACTIONS='1', TF3_DEBUG_FX='1', TF3_FORCE_COPIES='3',
                      TF3_FORCE_CARDS=str(card) if set_card else '4009,7125', TF_PRIMEIRO='0', TF_DECK_J1='1')
    os.environ.pop('TF3_DEBUG_LP', None)
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards')
    with open(os.path.join(pack, 'ported.txt'), encoding='utf-8') as f:
        gids = {int(r[1]): int(r[0]) for line in f if (r := line.split(' ', 3)) and len(r) == 4}
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        count = state()['hand']
        tf.run(' '.join(['LEFT w30'] * (count - 1)) + ' X w60 X w200 shot:played')
        query = Probe()
        monster_slot = next((i for i, e in enumerate(state()['field'][:5]) if e[0] & 0x1fff == 4009), None)
        base_stats = query.current_stats(0, monster_slot) if not set_card else None
        if not set_card:
            tf.run('RIGHT w40 X w60 X w200 X w80 X w200 shot:equipped')
        before = state()
        equipped_stats = query.current_stats(0, monster_slot) if not set_card else None
        if not set_card and tuple(a - b for a, b in zip(equipped_stats, base_stats)) != (500, 0, 0):
            raise RuntimeError('Naturally equipped Axe did not add exactly 500 ATK')
        source = next(i for i, e in enumerate(before['field']) if e[0] & 0x1fff == gids[card])
        if set_card:
            if before['field'][source][4] & 0x80: raise RuntimeError('Expected a Set equip fixture')
        elif not before['field'][source][1] or not before['field'][source][4] & 0x80:
            raise RuntimeError('Expected a face-up equipped fixture')
        removal = Probe().remove_current(0, source, destination=15 if args.case == 'banish' else 14,
                        effect=5350 if args.case == 'send' else 4909,
                        actor_side=0 if args.case == 'own' else 1, destroyed=args.case != 'send')
        tf.run('w300 shot:removed O w60 RIGHT w20 RIGHT w20 RIGHT w20 X w1500 X w100 X w300 shot:resolved')
        after = state()
        draw_count = len(re.findall(r'draw effect of id 4317: controller 0 draws 1, other player draws 0', tf.log_texto()))
        expected_draw = int(args.card == 'axe' and args.case == 'opponent')
        expected_damage = 600 if args.card == 'tank' and args.case not in ('send', 'banish') else 0
        gone = all(e[0] & 0x1fff != gids[card] for e in after['field'])
        result = dict(card=card, case=args.case, removal=removal, before=before, after=after, draw_events=draw_count,
                      expected_draw_events=expected_draw, expected_damage=expected_damage,
                      base_stats=base_stats, equipped_stats=equipped_stats)
        result['passed'] = removal == 1 and gone and draw_count == expected_draw and after['opponent_lp'] == before['opponent_lp'] - expected_damage and (not expected_draw or after['deck'] < before['deck'])
        with open(os.path.join(tf.CTL, 'equip_event_results.json'), 'w', encoding='utf-8') as f: json.dump(result, f, indent=2)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    sys.exit(main())
