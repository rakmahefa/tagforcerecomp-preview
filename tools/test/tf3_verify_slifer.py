"""Check Slifer the Sky Dragon's ATK / DEF (hand size x 1000) through the real duel engine.

Needs the test-only TF3TestProbe plugin next to TF3Cards (see tf3_verify_stats.py): run with TF_TEST_EXE / TF_MODS_FROM / TF_TAG set.
The hand size word of each side is poked, then the stats are queried with a field fixture that holds Slifer face-up.
  TF_TAG=sl TF_MODS_FROM=gen/tf3slifer TF_TEST_DATA=<game data> python -I tools/test/tf3_verify_slifer.py
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'mods'))
import tf
from tf3_probe_client import Probe, field

SIDES, SIZE = 0x09f79cec, 4012   # duel state: side 0 block, bytes per side; the hand size is the word at +12
SLIFER = 4999                    # TF3 card id (column 2 of ported.txt)


def main():
    os.environ['TF3_TEST_PROBE'] = '1'
    pack = os.path.join(os.environ['TF_MODS_FROM'], 'TF3Cards')
    rows = [x.split(' ', 3) for x in open(os.path.join(pack, 'ported.txt'), encoding='utf-8') if x.strip()]
    gid = {int(x[1]): int(x[0]) for x in rows}[SLIFER]
    results = []
    tf.duelo(['--lento'])
    try:
        probe = Probe()
        for side in (0, 1):
            addr = SIDES + side * SIZE + 12
            original = struct.unpack('<I', bytes.fromhex(tf.send('peek %08x 4' % addr).split()[-1]))[0]
            for n in (0, 1, 3, 5, 7):
                tf.send('poke %08x %s' % (addr, struct.pack('<I', n).hex()))
                atk, defe, _ = probe.stats([field(side, 0, gid)], side)
                ok = (atk, defe) == (n * 1000, n * 1000)
                results.append(ok)
                print('PASS' if ok else 'FAIL', 'side', side, 'hand', n, '-> ATK', atk, 'DEF', defe, flush=True)
            tf.send('poke %08x %s' % (addr, struct.pack('<I', original).hex()))
    finally:
        tf.stop()
    return 0 if all(results) else 1


if __name__ == '__main__':
    sys.exit(main())
