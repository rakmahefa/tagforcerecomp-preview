"""Record the native field state after the player Sets Magic Jammer."""
import json
import os
import struct
import tf
from tf3_verify_sabre import read


def main():
    os.environ.update(TF3_FORCE_COPIES='3', TF3_FORCE_CARDS='4862', TF_PRIMEIRO='0', TF_DECK_J1='1')
    try:
        tf.duelo([])
        tf.run('w300 O w60 DOWN w40 DOWN w40')
        count = struct.unpack('<I', read(0x09f79cf8, 4))[0]
        tf.run(' '.join(['LEFT w30'] * (count - 1)) + ' X w60 X w300 shot:set')
        slots = [struct.unpack('<5I', read(0x09f79cec + 0x30 + slot * 20, 20)) for slot in range(5, 10)]
        found = [(slot + 5, row) for slot, row in enumerate(slots) if (row[0] & 0x1fff) == 4862]
        print(json.dumps(dict(found=found, passed=bool(found))), flush=True)
        return 0 if found else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    raise SystemExit(main())
