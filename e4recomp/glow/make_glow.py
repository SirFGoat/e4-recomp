"""Bake neon glow into E4 sprite replacements.

For each texture in GLOW_SETS that has a dump (textures/dump), take its HD replacement
(textures/replace/esrgan, else the dump upscaled 4x), shrink the shape into the middle of
the canvas and paint a soft glow around it. The game draws these sprites additively on
black, so the glow simply adds light. e4 enlarges these sprites' quads by the same factor
(textures/replace/glow/glow_scale.txt), so shapes keep their on-screen size and the glow
spills out around them. HUD, text and everything else are left alone.

Usage: python make_glow.py [textures_dir]
Re-run after playing more stages with texture dumping on to cover new sprites.
"""
import csv
import fnmatch
import os
import sys

import numpy as np
from PIL import Image

TEX = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), '..', '..', 'textures')
TEX = os.path.abspath(TEX)

# name patterns -> glow preset
STRONG = dict(margin=0.17, tight_radius=0.012, tight=0.75, haze_radius=0.04, haze=0.55)
MEDIUM = dict(margin=0.17, tight_radius=0.010, tight=1.1, haze_radius=0.03, haze=0.45)
GLOW_SETS = [
    (['Enemy*', 'submarine*', 'satellite*', 'sg01satellite*', 'battleship*', 'Bee_*', 'St*Boss*'], STRONG),
    (['ExplosionSt*', 'ExplosionS4*', 'ExplosionVSMP*', 'GenericExplosionRing*'], STRONG),
    (['E4Title.bmp'], dict(MEDIUM, margin_px=0.45)),  # thin banner: margin from its height
]


def gaussian_blur(img, sigma):
    """Separable-equivalent gaussian via FFT on an HxWxC float array (zero padded)."""
    if sigma <= 0.5:
        return img.copy()
    pad = int(3 * sigma) + 1
    h, w = img.shape[:2]
    H, W = h + 2 * pad, w + 2 * pad
    y = np.fft.fftfreq(H)[:, None]
    x = np.fft.rfftfreq(W)[None, :]
    kernel = np.exp(-2 * (np.pi ** 2) * (sigma ** 2) * (x ** 2 + y ** 2))
    out = np.empty_like(img)
    for c in range(img.shape[2]):
        p = np.zeros((H, W), np.float32)
        p[pad:pad + h, pad:pad + w] = img[..., c]
        out[..., c] = np.fft.irfft2(np.fft.rfft2(p) * kernel, s=(H, W))[pad:pad + h, pad:pad + w]
    return np.clip(out, 0, None)


def bake(src, preset):
    w, h = src.size
    if 'margin_px' in preset:
        m = int(round(h * preset['margin_px']))  # same glow room on all sides
        mx = my = m
    else:
        mx, my = int(round(w * preset['margin'])), int(round(h * preset['margin']))
    W, H = w + 2 * mx, h + 2 * my  # canvas keeps the shape at its full resolution
    canvas = np.zeros((H, W, 3), np.float32)
    rgba = np.asarray(src.convert('RGBA'), np.float32) / 255.0
    canvas[my:my + h, mx:mx + w] = rgba[..., :3]
    # Glow source: the sprite with its own border feathered, so art that runs to
    # the texture edge (explosion rays) doesn't glow as a hard square.
    feather = max(2, int(min(w, h) * 0.06))
    fx = np.clip(np.minimum(np.arange(w), np.arange(w)[::-1]) / feather, 0, 1)
    fy = np.clip(np.minimum(np.arange(h), np.arange(h)[::-1]) / feather, 0, 1)
    glow_src = np.zeros_like(canvas)
    glow_src[my:my + h, mx:mx + w] = rgba[..., :3] * (fy[:, None] * fx[None, :])[..., None]
    size = max(w, h) if 'margin_px' not in preset else h * 4
    glow = (gaussian_blur(glow_src, preset['tight_radius'] * size) * preset['tight'] +
            gaussian_blur(glow_src, preset['haze_radius'] * size) * preset['haze'])
    # Screen blend: light pours around the shape without flattening its detail.
    out = 1.0 - (1.0 - canvas) * (1.0 - np.clip(glow, 0, 1))
    # Fade the glow to black at the very border so the quad edge never shows.
    fx = np.minimum(np.arange(W), np.arange(W)[::-1]) / max(1, mx * 0.35)
    fy = np.minimum(np.arange(H), np.arange(H)[::-1]) / max(1, my * 0.35)
    edge = np.clip(np.minimum(fy[:, None], fx[None, :]), 0, 1)[..., None]
    out = out * edge
    img = Image.fromarray((out * 255 + 0.5).astype(np.uint8), 'RGB').convert('RGBA')
    return img, W / w, H / h


def main():
    names = os.path.join(TEX, 'texture_names.csv')
    if not os.path.exists(names):
        sys.exit(f'{names} missing: start the game once (it writes this file at boot)')
    rows = list(csv.DictReader(open(names)))
    dumps = {}
    for f in os.listdir(os.path.join(TEX, 'dump')):
        if f.endswith('.png'):
            dumps[f[:-4].rsplit('_', 1)[-1]] = f
    for row in rows:
        row['dump'] = dumps.get(row['hash'], '')
    esrgan = os.path.join(TEX, 'replace', 'esrgan')
    # "@glow" is a replacement variant: e4 uses it (over the plain HD PNGs) only
    # with --texture_variants=glow, so glow can be switched off in the launcher.
    out_dir = os.path.join(TEX, 'replace', '@glow')
    os.makedirs(out_dir, exist_ok=True)
    # Older versions of this script moved the HD sources aside; put them back.
    parked = os.path.join(TEX, 'replace_parked')
    if os.path.isdir(parked):
        for f in os.listdir(parked):
            os.replace(os.path.join(parked, f), os.path.join(esrgan, f))
        os.rmdir(parked)
    old_out = os.path.join(TEX, 'replace', 'glow')
    if os.path.isdir(old_out):
        for f in os.listdir(old_out):
            os.remove(os.path.join(old_out, f))
        os.rmdir(old_out)
    scale_lines = []
    done = set()
    for row in rows:
        name, dump, hash_ = row['name'], row['dump'], row['hash']
        preset = next((p for pats, p in GLOW_SETS if any(fnmatch.fnmatch(name, pat) for pat in pats)), None)
        if preset is None or hash_ in done:
            continue
        done.add(hash_)
        hd = os.path.join(esrgan, dump) if dump else ''
        if dump and os.path.exists(hd):
            src = Image.open(hd)
        elif dump and os.path.exists(os.path.join(TEX, 'dump', dump)):
            d = Image.open(os.path.join(TEX, 'dump', dump))
            src = d.resize((d.width * 4, d.height * 4), Image.LANCZOS)
        else:
            print(f'  skip {name}: not dumped yet')
            continue
        img, kx, ky = bake(src, preset)
        out_name = dump if dump else f'{row["size"]}_{hash_}.png'
        img.save(os.path.join(out_dir, out_name))
        scale_lines.append(f'{hash_} {kx:.5f} {ky:.5f} {name}')
        print(f'  {name:26} {src.width}x{src.height} -> {img.width}x{img.height} (quad x{kx:.3f}, x{ky:.3f})')
    with open(os.path.join(out_dir, 'glow_scale.txt'), 'w') as f:
        f.write('# hash scale_x scale_y name  (read by e4: enlarge these sprites\' quads)\n')
        f.write('\n'.join(scale_lines) + '\n')
    print(f'{len(scale_lines)} glow sprites written to {out_dir}')


if __name__ == '__main__':
    main()
