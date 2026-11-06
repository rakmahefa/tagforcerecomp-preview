#!/usr/bin/env python3
"""Makes a synthetic PSP-style disc image for testing the launcher's Disc page.

    python3 tools/launcher/test/make_test_iso.py [out.iso] [--id ULES-00600]

The image has the ISO9660 primary volume descriptor, a root directory with
UMD_DATA.BIN, and the game id line inside it - exactly the parts the launcher
reads (tools/launcher/launcher_iso.cpp, mirroring runtime/core/gameid.cpp).
It contains NO game data: 45 KB of zeros with an id in them. With --id the
id can be changed, to test that a wrong disc is refused.
"""
import struct
import sys


def drec(lba, size, flags, ident: bytes) -> bytes:
    n = len(ident)
    pad = (33 + n) % 2
    rec = bytearray()
    rec += b'\x00'  # record length, set at the end
    rec += b'\x00'  # extended attributes
    rec += struct.pack('<I', lba) + struct.pack('>I', lba)
    rec += struct.pack('<I', size) + struct.pack('>I', size)
    rec += bytes(7)  # recording time
    rec += bytes([flags])
    rec += b'\x00\x00'  # file unit size, interleave gap
    rec += struct.pack('<HH', 1, 1)  # volume sequence number
    rec += bytes([n]) + ident + bytes(pad)
    rec[0] = len(rec)
    return bytes(rec)


def main():
    args = sys.argv[1:]
    out = 'test.iso'
    game_id = 'ULES-00600'
    if args and not args[0].startswith('--'):
        out = args.pop(0)
    if '--id' in args:
        game_id = args[args.index('--id') + 1]
    if len(game_id) != 10 or game_id[4] != '-':
        sys.exit('the id looks like ULES-00600 (10 characters, a dash after 4)')

    sectors = [bytearray(2048) for _ in range(22)]
    # Sector 16: the primary volume descriptor, root directory at sector 20.
    pvd = sectors[16]
    pvd[0] = 1
    pvd[1:6] = b'CD001'
    pvd[6] = 1
    pvd[40:72] = b'TAGFORCE'.ljust(32, b' ')
    root = drec(20, 2048, 0x02, b'\x00')
    pvd[156:156 + len(root)] = root
    # Sector 20: the root directory, with UMD_DATA.BIN pointing at sector 21.
    at = 0
    for record in (drec(20, 2048, 0x02, b'\x00'), drec(20, 2048, 0x02, b'\x00'),
                   drec(21, 36, 0x00, b'UMD_DATA.BIN')):
        sectors[20][at:at + len(record)] = record
        at += len(record)
    # Sector 21: the id line the game id is read from.
    sectors[21][:36] = (game_id + '|0123456789ABCDEF|0001|G').encode()

    with open(out, 'wb') as f:
        for sector in sectors:
            f.write(sector)
    print('wrote %s (%s)' % (out, game_id))


if __name__ == '__main__':
    main()
