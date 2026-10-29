"""Client for the opt-in TF3TestProbe; all state belongs to one TF_TAG session."""
import re
import struct
import tf


def field(side, slot, cid, link=0, enabled=1, flags=0x80):
    return [side, slot, cid | 0x00800000, link, enabled, 0, flags]


class Probe:
    def __init__(self):
        match = re.search(r'TF3 test mailbox ([0-9a-fA-F]+)', tf.log_texto())
        if not match:
            raise RuntimeError('TF3TestProbe did not publish its mailbox')
        self.mailbox = int(match.group(1), 16)
        self.sequence = 0

    def query(self, fixtures, side, slot=0, operation=0, attacker=(0, 0), check_protection=1):
        self.sequence += 1
        header = [0, self.sequence, 0, 0, side, slot, len(fixtures), operation] + [0] * 16
        header += [*attacker, check_protection] + [0] * 5
        data = struct.pack('<32I', *header) + b''.join(struct.pack('<7I', *f) for f in fixtures)
        tf.send('poke %08x %s' % (self.mailbox, data.hex()))
        for _ in range(30):
            tf.send('step 1')
            response = bytes.fromhex(tf.send('peek %08x 96' % self.mailbox).split()[-1])
            words = struct.unpack('<24I', response)
            if words[2] == self.sequence:
                break
        if words[2] != self.sequence or words[3]:
            raise RuntimeError('probe failed: sequence %s status %s' % (words[2], words[3]))
        return response[32:96]

    def stats(self, fixtures, side):
        response = self.query(fixtures, side)
        return struct.unpack_from('<ii', response, 20) + (struct.unpack_from('<i', response, 16)[0],)

    def current_stats(self, side, slot):
        response = self.query([], side, slot, operation=7)
        return struct.unpack_from('<ii', response, 20) + (struct.unpack_from('<i', response, 16)[0],)

    def destruction_marks(self, side, slot, mask=0, effect=4909, actor_side=1):
        return struct.unpack_from('<I', self.query([], side, slot, operation=8,
                attacker=(effect, int(actor_side != side)), check_protection=mask))[0]

    def destroy_group(self, mask, effect=4909, actor_side=1):
        return struct.unpack_from('<I', self.query([], 0, operation=9,
                attacker=(effect, actor_side), check_protection=mask))[0]

    def mill_current(self, side, count=1):
        self.query([], side, operation=10, attacker=(count, 0))

    def attack(self, fixtures, target_side, check_protection=1):
        return struct.unpack_from('<I', self.query(fixtures, target_side, operation=1, check_protection=check_protection))[0]

    def control(self, fixtures, target_side):
        return struct.unpack_from('<I', self.query(fixtures, target_side, operation=2))[0]

    def flip_kind(self, cid):
        return struct.unpack_from('<I', self.query([], 0, operation=3, attacker=(cid, 0)))[0]

    def current_position_permission(self, side, slot):
        return struct.unpack_from('<II', self.query([], side, slot, operation=4))

    def equip_target(self, fixtures, source, target_side, source_side=0):
        return struct.unpack_from('<I', self.query(fixtures, target_side, operation=5, attacker=(source, source_side)))[0]

    def remove_current(self, side, slot, destination=14, effect=4909, actor_side=1, destroyed=True, battle=False, tributed=False):
        """Explicit action, not restored; requires TF3_TEST_ACTIONS in the isolated test process."""
        return struct.unpack_from('<I', self.query([], side, slot, operation=6,
                attacker=(effect, destination), check_protection=(2 if destroyed else 0) | int(actor_side != side) | (0x10000 if battle else 0) | (0x1000 if tributed else 0)))[0]

