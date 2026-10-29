"""Run the opt-in pending-trigger engine probe in an isolated TF_TAG session."""
import json
import os
import re
import struct
import sys
import tf


def main():
    os.environ['TF3_TEST_PROBE'] = '1'
    os.environ['TF3_EVENT_IDS'] = '1'
    try:
        tf.duelo([])
        match = re.search(r'TF3 trigger mailbox ([0-9a-fA-F]+)', tf.log_texto())
        if not match:
            raise RuntimeError('TF3TriggerProbe did not publish its mailbox')
        mailbox = int(match.group(1), 16)
        rounds = []
        for sequence in (1, 2):
            tf.send('poke %08x %s' % (mailbox + 4, struct.pack('<I', sequence).hex()))
            for _ in range(30):
                tf.send('step 1')
                words = struct.unpack('<5I', bytes.fromhex(tf.send('peek %08x 20' % mailbox).split()[-1]))
                if words[2] == sequence:
                    break
            rounds.append(dict(completed=words[2] == sequence, checks=words[4], failures=words[3]))
        result = dict(rounds=rounds, checks=sum(r['checks'] for r in rounds), failures=sum(r['failures'] for r in rounds))
        result['passed'] = all(r['completed'] and r['checks'] >= 60 and r['failures'] == 0 for r in rounds)
        with open(os.path.join(tf.CTL, 'trigger_results.json'), 'w', encoding='utf-8') as f:
            json.dump(result, f, indent=2)
        print(json.dumps(result), flush=True)
        return 0 if result['passed'] else 1
    finally:
        tf.stop()


if __name__ == '__main__':
    sys.exit(main())
