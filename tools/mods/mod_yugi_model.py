"""Recolour the private Yugi duel model (a Zane copy) by editing the palette of its texture.

The texture `ryo01.gim` is 8-bit indexed, so changing palette entries recolours every pixel that uses them and leaves the
mesh, UVs and animations untouched. Hair (teal) becomes black with a purple tint; the white coat becomes a Domino-uniform blue.

The archive is patched in place (same size and layout), so the game's parser sees an ordinary EHP. Repacking with
ehp.py changes the size of the gzip members and was observed to crash the duel, so do not repack.

Usage:
  python tools/mods/mod_yugi_model.py <source cutin_ryo01.ehp> <out cutin_yugi01.ehp>
"""
import colorsys
import struct
import sys

PALETTE_AT, PALETTE_BYTES = 0x80, 1024      # RGBA8888 x 256, in the GIM palette block
KEEP = {251, 252, 31, 30, 255, 32, 227, 249}  # teal-ish greys of the disk/cuff, not hair


def lerp(a, b, t):
    t = max(0.0, min(1.0, t))
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def recolour(pal):
    out = bytearray(pal)
    for i in range(256):
        r, g, b, a = pal[i * 4:i * 4 + 4]
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        new = None
        if i not in KEEP and 0.49 <= h <= 0.58 and 0.17 <= s <= 0.55 and v < 0.5:
            new = lerp((6, 4, 16), (48, 28, 82), (v - 0.03) / 0.45)             # hair
        elif s < 0.06 and v > 0.5 and i not in KEEP:
            new = lerp((22, 34, 100), (72, 104, 196), (v - 0.5) / 0.5)           # coat
        if new:
            out[i * 4:i * 4 + 3] = bytes(new)
    return bytes(out)


def main():
    src, out = sys.argv[1], sys.argv[2]
    d = bytearray(open(src, "rb").read())
    if d[:4] != b"EHP\x03":
        sys.exit("not an EHP archive")
    count = struct.unpack_from("<I", d, 12)[0]
    for k in range(count):
        name_off, data_off = struct.unpack_from("<II", d, 16 + 8 * k)
        end = d.index(b"\x00", name_off)
        if d[name_off:end] != b"ryo01.gim":
            continue
        size = struct.unpack_from("<I", d, end + 1)[0]
        at = data_off + PALETTE_AT
        d[at:at + PALETTE_BYTES] = recolour(bytes(d[at:at + PALETTE_BYTES]))
        open(out, "wb").write(d)   # same size and layout as the source: only palette bytes change
        print("recoloured ryo01.gim (%d bytes) -> %s" % (size, out))
        return
    sys.exit("ryo01.gim not found")


if __name__ == "__main__":
    main()
