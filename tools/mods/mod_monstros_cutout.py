"""Cut-out sprites for the "Monsters on the field" mod: removes the art background with SAM (facebook/sam-vit-base, Apache-2.0).
Usage: python tools/mods/mod_monstros_cutout.py [texture_pack] [mod_folder] [id ...] [--preview]
  Reads cards/large/<id>.png from the texture pack (read only), writes <mod_folder>/sprites/<id>.rgba (cropped to the monster, so w/h vary)
  and, with --preview, gen/monstros_preview/<id>.png (monster over a dark purple background) to judge the result.
  The model is cached in gen/hf_cache (HF_HOME is set here; nothing from the game or the model goes into git).
.rgba format: w, h (uint32 LE) then w*h RGBA pixels (straight alpha)."""
import os, struct, sys
RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
os.environ.setdefault('HF_HOME', os.path.join(RAIZ, 'gen', 'hf_cache'))
import numpy as np
import torch
from PIL import Image, ImageFilter
from transformers import SamModel, SamProcessor

ART = (0, 0, 312, 240)
DECK = [4041, 4766, 5649, 5880, 4008, 4064, 5835, 6000, 4028, 5655, 4913, 4597, 5091, 4054, 4762]


def largest_component(m):
    """Keeps the connected region (4-neighbours) that has the most pixels; no scipy: iterative flood fill on a small image."""
    h, w = m.shape; lab = np.zeros((h, w), np.int32); best, best_n, n = 0, 0, 0
    for sy in range(h):
        for sx in range(w):
            if m[sy, sx] and not lab[sy, sx]:
                n += 1; stack = [(sy, sx)]; lab[sy, sx] = n; cnt = 0
                while stack:
                    y, x = stack.pop(); cnt += 1
                    for yy, xx in ((y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)):
                        if 0 <= yy < h and 0 <= xx < w and m[yy, xx] and not lab[yy, xx]:
                            lab[yy, xx] = n; stack.append((yy, xx))
                if cnt > best_n: best, best_n = n, cnt
    return lab == best


def segment(model, proc, dev, im):
    """SAM with a box over the central area plus a centre point; picks the mask with the best score that is neither tiny nor the whole picture."""
    w, h = im.size
    box = [[[w * 0.05, h * 0.03, w * 0.95, h * 0.97]]]
    pts = [[[[w * 0.5, h * 0.5]]]]
    inp = proc(im, input_boxes=box, input_points=pts, return_tensors='pt').to(dev)
    with torch.no_grad(): out = model(**inp, multimask_output=True)
    masks = proc.image_processor.post_process_masks(out.pred_masks.cpu(), inp['original_sizes'].cpu(), inp['reshaped_input_sizes'].cpu())[0][0]   # (3,H,W)
    scores = out.iou_scores.cpu()[0][0]
    cand = []
    for i in range(masks.shape[0]):
        m = masks[i].numpy().astype(bool); frac = m.mean()
        if 0.08 < frac < 0.80: cand.append((float(scores[i]), i, m))
    if not cand:
        i = int(scores.argmax()); return masks[i].numpy().astype(bool)
    return max(cand)[2]


HOLO = np.array([120, 200, 255], np.float32)   # hologram tint / glow colour


def holo_look(rgb, m, k):
    """Soft hologram look: the mask is eroded (drops the background halo), blurred into a wide gradient edge, the base fades out smoothly and a blurred
    cyan glow surrounds the monster. k = atlas scale (1 = 512 wide, 2 = HD). Returns an RGBA array cropped to the visible area."""
    pad = int(40 * k)
    rgb = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode='edge'); m = np.pad(m, pad)
    mk = Image.fromarray((m * 255).astype(np.uint8))
    core = mk.filter(ImageFilter.MinFilter(2 * int(3 * k) + 1)).filter(ImageFilter.GaussianBlur(5 * k))           # soft edge, 5 px wide
    a = np.asarray(core).astype(np.float32) / 255
    ys = np.where(m.any(1))[0]; y0, y1 = ys.min(), ys.max()
    t = np.clip((y1 - np.arange(m.shape[0])) / max(1.0, 0.22 * (y1 - y0)), 0, 1); t = t * t * (3 - 2 * t)   # base fade (smoothstep over the lowest 22%)
    a = a * t[:, None]
    halo = np.asarray(mk.filter(ImageFilter.MaxFilter(2 * int(6 * k) + 1)).filter(ImageFilter.GaussianBlur(14 * k))).astype(np.float32) / 255 * 0.55 * t[:, None]
    col = rgb.astype(np.float32) * 0.92 + HOLO * 0.08
    oa = a + halo * (1 - a)
    oc = (col * a[..., None] + HOLO * (halo * (1 - a))[..., None]) / np.maximum(oa, 1e-4)[..., None]
    out = np.dstack([np.clip(oc + 0.5, 0, 255), np.clip(oa * 255 + 0.5, 0, 255)]).astype(np.uint8)
    yy, xx = np.where(out[..., 3] > 6)
    return np.ascontiguousarray(out[yy.min():yy.max() + 1, xx.min():xx.max() + 1])


def cutout(model, proc, dev, pacote, saida, cid, preview):
    f = os.path.join(pacote, 'cards', 'large', '%d.png' % cid)
    if not os.path.exists(f): print('no art', cid); return False
    im = Image.open(f).convert('RGB'); k = im.width / 512.0   # the HD pack atlas is 2x the original 512x256
    im = im.crop(tuple(int(v * k) for v in ART))
    m = segment(model, proc, dev, im)
    m = largest_component(m)
    rgba = holo_look(np.asarray(im), m, k)
    with open(os.path.join(saida, 'sprites', '%d.rgba' % cid), 'wb') as g:
        g.write(struct.pack('<II', rgba.shape[1], rgba.shape[0])); g.write(rgba.tobytes())
    if preview:
        os.makedirs(os.path.join(RAIZ, 'gen', 'monstros_preview'), exist_ok=True)
        bg = Image.new('RGBA', (rgba.shape[1], rgba.shape[0]), (40, 16, 70, 255)); bg.alpha_composite(Image.fromarray(rgba))
        bg.save(os.path.join(RAIZ, 'gen', 'monstros_preview', '%d.png' % cid))
    print('ok', cid, rgba.shape[1], 'x', rgba.shape[0], 'cover %.0f%%' % (m.mean() * 100)); return True


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    pacote = a[0] if a else os.path.join(RAIZ, 'mods', 'EnhancedTextures', 'textures')
    saida = a[1] if len(a) > 1 else os.path.join(RAIZ, 'mods', 'MonstersOnField')
    ids = [int(x) for x in a[2:]] or DECK
    os.makedirs(os.path.join(saida, 'sprites'), exist_ok=True)
    dev = 'cuda' if torch.cuda.is_available() else 'cpu'
    model = SamModel.from_pretrained('facebook/sam-vit-base').to(dev).eval(); proc = SamProcessor.from_pretrained('facebook/sam-vit-base')
    n = sum(cutout(model, proc, dev, pacote, saida, i, '--preview' in sys.argv) for i in ids)
    print('%d sprite(s) in %s' % (n, os.path.join(saida, 'sprites')))
