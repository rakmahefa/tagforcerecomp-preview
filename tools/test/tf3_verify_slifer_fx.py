"""Duel checks of Slifer the Sky Dragon's effects (the ATK / DEF rule has its own script, tf3_verify_slifer.py).

Three isolated duels through tf3_lote.py; Slifer is poked into a field zone (it does not render, which is enough for the engine) and the plugin's debug log is read:
  summon   an opponent monster with 2000 DEF or less appears while Slifer is face-up -> it is destroyed (Graveyard of the summoner)
  special  Slifer appears during the action of another card (= Special Summoned) -> it goes to the Graveyard when the turn ends
  target   Change of Heart tests Slifer and a plain monster as targets -> Slifer is refused, the other one is accepted
  tribute3 Slifer in hand, 3 monsters poked into your zones: Summon asks for 3 Tributes and all 3 go to the Graveyard
  set3     the same with Set
  tribute2 only 2 monsters: the hand menu offers no Summon / Set (the first X opens Show Details), nothing moves
  obelisk3 Obelisk the Tormentor: 3 Tributes as tribute3
  ra3      The Winged Dragon of Ra: 3 Tributes (Sangan 1000 / 600 each) -> ATK 3000 DEF 1800
Run (no -I): TF_TEST_EXE=<package exe> TF_TEST_DATA=<game data> python tools/test/tf3_verify_slifer_fx.py [case ...]
"""
import glob
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
MODS = os.environ.get('TF_MODS_FROM', 'gen/tf3slifer')
SLIFER = 7025   # duel id of TF3 card 4999 (column 1 of ported.txt)


def zone(addr, card):   # poke words that put a face-up monster into a field zone (entry address, game id)
    return ('poke:%08x:%s poke:%08x:0100 poke:%08x:80000000' % (addr, (card | 0x800000).to_bytes(4, 'little').hex(), addr + 8, addr + 16))


SIDE0, SIDE1 = 0x09f79d1c, 0x09f7acc8   # first monster zone of each side
T1 = 'w300 estado:ini ' + ' '.join(zone(SIDE0 + 20 * k, 4054) for k in range(3)) + ' w30 DOWN w40 DOWN w40 LEFT w30 LEFT w30 LEFT w30 LEFT w30 LEFT w30 X w100 '   # turn 1, Slifer = hand slot 0
PICKS = 'X w300 X w300 RIGHT w30 X w300 X w300 w100 X w300 w600 estado:fim'   # Summon / Set, YES, three picks (the cursor returns to the first candidate after each)
HAND = 'w60 DOWN w40 DOWN w40 LEFT w30 LEFT w30 LEFT w30 LEFT w30 LEFT w30 X w100 '
CASES = {
    'summon': dict(card=4012, env={'TF3_DEBUG_SLIFER': '1'},
                   rot='w300 estado:ini ' + zone(SIDE1, SLIFER) + ' ' + HAND + 'X w300 w600 estado:fim',
                   log=['appeared, DEF 1400 -> destroy'], grave_me=True),
    'special': dict(card=4012, env={'TF3_DEBUG_SLIFER': '1'},
                    rot='w300 estado:ini ' + zone(SIDE0, SLIFER) + ' ' + HAND + 'X w600 O w60 RIGHT w20 RIGHT w20 RIGHT w20 X w3000 estado:fim',
                    log=['-> Special Summon', 'End Phase, sending the Special Summoned card'], grave_has=SLIFER),
    'target': dict(card=4860, env={'TF3_DEBUG_TARGET': '1'},
                   rot='w300 estado:ini ' + zone(SIDE1, SLIFER) + ' ' + zone(SIDE1 + 20, 4012) + ' ' + HAND + 'X w300 w300 estado:fim',
                   log=['slot 0 card %d -> refused (God)' % SLIFER, 'slot 1 card 4012\n']),
    'tribute3': dict(card=4999, env={'TF3_DEBUG_SLIFER': '1'}, rot=T1 + PICKS, log=['god: third Tribute'], grave_n=3),
    'set3': dict(card=4999, env={'TF3_DEBUG_SLIFER': '1'}, rot=T1 + 'DOWN w30 ' + PICKS, log=['god: third Tribute'], grave_n=3),
    'tribute2': dict(card=4999, env={}, rot=T1.replace(' ' + zone(SIDE0 + 40, 4054), '') + 'X w300 X w300 w300 estado:fim', log=[], grave_n=0, hand_same=True),
    'obelisk3': dict(card=4998, env={'TF3_DEBUG_SLIFER': '1'}, rot=T1 + PICKS, log=['god: third Tribute'], grave_n=3),
    'ra3': dict(card=5000, env={'TF3_DEBUG_SLIFER': '1'}, rot=T1 + PICKS, log=['god: third Tribute', 'gets ATK 3000 DEF 1800'], grave_n=3),
}


def run(name, case):
    env = dict(os.environ, TF_MODS_FROM=MODS, **case['env'])
    subprocess.run([sys.executable, 'tools/test/tf3_lote.py', '-j', '1', '--mods', MODS, '--rot', case['rot'], str(case['card'])],
                   cwd=ROOT, env=env, capture_output=True, text=True, timeout=700)
    logs = sorted(glob.glob(os.path.join(ROOT, 'gen', 'logs_teste_l%d' % case['card'], 'sessao_*.log')), key=os.path.getmtime)
    text = open(logs[-1], encoding='utf-8', errors='replace').read() if logs else ''
    ok = all(s in text for s in case['log'])
    state = json.load(open(os.path.join(ROOT, 'gen', 'lote', str(case['card']), 'state.json'), encoding='utf-8'))
    if case.get('grave_me'):
        ok = ok and len(state['fim']['me']['grave']) > len(state['ini']['me']['grave'])
    if 'grave_n' in case:
        ok = ok and len(state['fim']['me']['grave']) - len(state['ini']['me']['grave']) == case['grave_n']
    if case.get('hand_same'):
        ok = ok and state['fim']['me']['n_hand'] == state['ini']['me']['n_hand']
    if case.get('grave_has'):
        ok = ok and case['grave_has'] in state['fim']['me']['grave']
    print('PASS' if ok else 'FAIL', name, flush=True)
    return ok


if __name__ == '__main__':
    wanted = sys.argv[1:] or list(CASES)
    results = [run(n, CASES[n]) for n in wanted]
    sys.exit(0 if all(results) else 1)
