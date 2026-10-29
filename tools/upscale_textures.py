"""Upscale the game's dumped textures for streets_fix's replacement loader.

Input:  game/textures/dump/<site>_<w>x<h>_<hash>.png   (written by texturedump=1)
Output: game/textures/hd/<hash>.png                     (loaded by the DLL in place of the original)

Each texture is prepared before the AI upscaler sees it:
  * transparent pixels (the game's colour key) get the colour of the nearest opaque pixel,
    so the upscaler doesn't pull a dark fringe into the edges;
  * it is padded: by wrapping for world textures (they tile across walls and ground, so the
    result stays seamless), by repeating the edge otherwise. The padding is cropped after.
Alpha is upscaled separately (smooth, then thresholded back to on/off, since the game only
draws fully opaque or fully transparent texels). Results are capped at --max-size per side.

Usage:
  python tools/upscale_textures.py [--model realesrgan-x4plus] [--max-size 512] [--only SITE ...]
Needs Pillow and numpy, and tools/realesrgan/realesrgan-ncnn-vulkan.exe.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DUMP = os.path.join(ROOT, "game", "textures", "dump")
HD = os.path.join(ROOT, "game", "textures", "hd")
UPSCALER = os.path.join(ROOT, "tools", "realesrgan", "realesrgan-ncnn-vulkan.exe")

WORLD_SITE = "004b6da1"  # 32x32 building/ground textures: tile
SKIP_SITES = {
    "004ae848",  # radar: redrawn every frame
    "004abb2d",  # font: glyph sheet, keyed on near-black; leave crisp
}
# Individual textures to leave alone. The game font's main sheet comes through the general
# texture loader (0x4b6b34) and is keyed at draw time (near-black -> transparent); upscaled,
# its soft edges stop matching the key and every capital letter gets a box of noise.
SKIP_KEYS = {
    "084b725b208fbb99",  # 004b6b34 256x256: font sheet (capitals, digits, symbols)
}
SCALE = 4


def parse_name(name):
    base = os.path.splitext(name)[0]
    site, size, key = base.split("_")
    w, h = (int(v) for v in size.split("x"))
    return site, w, h, key


def fill_transparent(rgb, opaque):
    """Give each transparent pixel the colour of a nearby opaque one (repeated 1-pixel dilation)."""
    if opaque.all() or not opaque.any():
        return rgb
    rgb = rgb.copy()
    known = opaque.copy()
    for _ in range(max(rgb.shape[:2])):
        if known.all():
            break
        acc = np.zeros(rgb.shape, np.float64)
        cnt = np.zeros(known.shape, np.float64)
        for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            k = np.roll(known, (dy, dx), (0, 1))
            c = np.roll(rgb, (dy, dx), (0, 1))
            acc += c * k[..., None]
            cnt += k
        grow = (~known) & (cnt > 0)
        rgb[grow] = (acc[grow] / cnt[grow][:, None]).astype(np.uint8)
        known |= grow
    return rgb


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", default="realesrgan-x4plus")
    ap.add_argument("--max-size", type=int, default=512)
    ap.add_argument("--only", nargs="*", help="call sites to process (default: all but radar and font)")
    ap.add_argument("--force", action="store_true", help="redo textures that already have an hd file")
    args = ap.parse_args()

    os.makedirs(HD, exist_ok=True)
    jobs = []
    for name in sorted(os.listdir(DUMP)):
        if not name.endswith(".png"):
            continue
        site, w, h, key = parse_name(name)
        if (args.only and site not in args.only) or (not args.only and site in SKIP_SITES):
            continue
        if key in SKIP_KEYS:
            continue
        if min(w, h) < 8:
            continue  # 2x2 / 256x1 lookup tables, nothing to gain
        if not args.force and os.path.exists(os.path.join(HD, key + ".png")):
            continue
        jobs.append((name, site, w, h, key))
    print(f"{len(jobs)} textures to upscale")
    if not jobs:
        return

    tmp = tempfile.mkdtemp(prefix="sosc_upscale_")
    src_dir, dst_dir = os.path.join(tmp, "in"), os.path.join(tmp, "out")
    os.makedirs(src_dir)
    os.makedirs(dst_dir)
    meta = {}
    for name, site, w, h, key in jobs:
        im = np.array(Image.open(os.path.join(DUMP, name)).convert("RGBA"))
        opaque = im[..., 3] > 0
        rgb = fill_transparent(im[..., :3], opaque)
        pad = max(4, min(w, h) // 4)
        mode = "wrap" if site == WORLD_SITE else "edge"
        padded = np.pad(rgb, ((pad, pad), (pad, pad), (0, 0)), mode=mode)
        Image.fromarray(padded).save(os.path.join(src_dir, key + ".png"))
        meta[key] = (w, h, pad, opaque)

    cmd = [UPSCALER, "-i", src_dir, "-o", dst_dir, "-n", args.model, "-s", str(SCALE), "-f", "png"]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)

    done = 0
    for key, (w, h, pad, opaque) in meta.items():
        path = os.path.join(dst_dir, key + ".png")
        if not os.path.exists(path):
            print("missing output for", key, file=sys.stderr)
            continue
        up = Image.open(path).convert("RGB")
        p = pad * SCALE
        up = up.crop((p, p, p + w * SCALE, p + h * SCALE))
        size = up.size
        if max(size) > args.max_size:
            f = args.max_size / max(size)
            size = (max(1, round(size[0] * f)), max(1, round(size[1] * f)))
            up = up.resize(size, Image.LANCZOS)
        alpha = Image.fromarray((opaque * 255).astype(np.uint8)).resize(size, Image.BICUBIC)
        alpha = alpha.point(lambda v: 255 if v >= 128 else 0)
        out = up.convert("RGBA")
        out.putalpha(alpha)
        out.save(os.path.join(HD, key + ".png"))
        done += 1
    shutil.rmtree(tmp, ignore_errors=True)
    print(f"wrote {done} textures to {HD}")


if __name__ == "__main__":
    main()
