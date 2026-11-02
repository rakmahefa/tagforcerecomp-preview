"""Pack a PNG atlas into the Yugi test model's EHP without changing other members.

Usage: python tools/mods/yugi_model_atlas.py <source.ehp> <atlas.png> <output.ehp> [--register PACK]
       python tools/mods/yugi_model_atlas.py --register PACK --ehp <installed.ehp> <atlas.png>
The source and output must be different paths. Archive offsets remain unchanged because
the replacement GIM retains its original size. This tool does not edit the 3D mesh.

The in-model texture is only a 256x256 fallback; the game shows the full-size atlas through
the texture pack. A pack with textures/textures.ini does not scan its folders for PNGs, so
every new in-model texture (new hash) must be registered there. --register PACK copies the
atlas to PACK/textures/characters/yugi_model/<hash>.png and replaces the Yugi model row in
the manifest. Without it the game falls back to the blocky 256x256 texture.
"""
import argparse
import shutil
import sys
from pathlib import Path

import xxhash
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "game"))
import ehp
from tf3_porte_atlas import _swizzle

ATLAS_DIR = "characters/yugi_model"


def texture_hash(gim):
    """Runtime replacement key of a 256x256 8-bit GIM (texel data hash, as texrepl.cpp)."""
    return f"{xxhash.xxh64(gim[0x4D0:], seed=0xBACD7814).intdigest() & 0xFFFFFFFF:08x}"


def register(pack, key, atlas):
    """Copy the atlas into the pack and point the manifest's single Yugi model row at it."""
    textures = pack / "textures"
    manifest = textures / "textures.ini"
    if not manifest.is_file():
        raise SystemExit(f"{manifest} not found")
    dest = textures / ATLAS_DIR / f"{key}.png"
    with Image.open(atlas) as image:
        if image.size != (4096, 4096):
            raise SystemExit(f"atlas must be 4096x4096, got {image.size}")
    if not dest.exists() or dest.resolve() != atlas.resolve():
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(atlas, dest)
    text = manifest.read_text(encoding="utf-8")
    lines = [l for l in text.splitlines() if f"= {ATLAS_DIR}/" not in l]
    lines.append(f"{'0' * 16}{key} = {ATLAS_DIR}/{key}.png")
    manifest.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"Registered {key} -> {ATLAS_DIR}/{key}.png in {manifest}")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("atlas", type=Path)
    parser.add_argument("output", type=Path, nargs="?")
    parser.add_argument("--register", type=Path, metavar="PACK", help="texture pack to register the atlas in")
    parser.add_argument("--ehp", type=Path, help="only register: hash this already packed model")
    args = parser.parse_args()
    if args.ehp:
        if not args.register:
            parser.error("--ehp needs --register")
        gim = dict(ehp.ler(args.ehp.read_bytes())[1])["ryo01.gim"]
        register(args.register, texture_hash(gim), args.atlas)
        return
    if args.source is None or args.output is None:
        parser.error("source and output are required")
    if args.source.resolve() == args.output.resolve():
        parser.error("source and output must be different files")
    if args.output.exists():
        parser.error("output already exists")
    source = args.source.read_bytes()
    tag, members = ehp.ler(source)
    original = dict(members)["ryo01.gim"]
    if len(original) != 66768 or original[:11] != b"MIG.00.1PSP":
        raise ValueError("unexpected source texture layout")
    with Image.open(args.atlas) as image:
        texture = image.convert("RGB").resize((256, 256), Image.Resampling.LANCZOS)
        texture = texture.quantize(colors=256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    palette = texture.getpalette()
    rgba = bytes(v for i in range(256) for v in (palette[i * 3], palette[i * 3 + 1], palette[i * 3 + 2], 255))
    gim = bytearray(original)
    gim[0x80:0x480] = rgba
    gim[0x4D0:] = _swizzle(texture.tobytes(), 256, 256)
    packed = ehp.montar([(name, bytes(gim) if name == "ryo01.gim" else data) for name, data in members], tag)
    if len(packed) != len(source):
        raise ValueError("archive layout changed")
    new_members = dict(ehp.ler(packed)[1])
    if any(new_members[name] != data for name, data in members if name != "ryo01.gim"):
        raise ValueError("an unrelated archive member changed")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(packed)
    key = texture_hash(bytes(gim))
    print(f"Wrote {args.output} ({len(packed)} bytes), in-model texture hash {key}")
    if args.register:
        register(args.register, key, args.atlas)
    else:
        print("WARNING: atlas not registered in any texture pack; pass --register PACK or the game shows 256x256")


if __name__ == "__main__":
    main()
