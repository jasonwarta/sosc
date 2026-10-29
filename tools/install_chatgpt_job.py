"""Check a finished ChatGPT texture job and install its results as HD replacements.

usage: python tools/install_chatgpt_job.py <job-name> [--flip] [--dry-run] [--only key ...]

For every out/<key>.png with a matching dump: checks the size (4x the original), reports how
well the layout matches (blurred-luminance correlation after downscaling to the original
size), the brightness change (flagged over 20 levels) and the wrap seams (edge difference over the mean neighbour difference; about 1 means
seamless), then installs it into game/textures/hd/<key>.png, first backing up whatever was
there to game/textures/hd_backup_esrgan/ (once; the first backup is the one kept).

Results with transparency get their alpha thresholded at 50% (the game's sprites are
cut-outs) and the colour of transparent pixels filled in from the nearest opaque ones, so
filtering doesn't pull a dark or white fringe into the edges. --flip turns results made
upright (make_chatgpt_job.py --flip) back into the game's row order.
"""
import glob
import os
import shutil
import sys

import numpy as np
from PIL import Image, ImageFilter

TEX = os.path.join(os.path.dirname(__file__), "..", "game", "textures")


def layout_corr(orig, result):
    a = orig.convert("L").filter(ImageFilter.GaussianBlur(1))
    b = result.convert("L").resize(orig.size, Image.BOX).filter(ImageFilter.GaussianBlur(1))
    a = np.asarray(a, float) - np.asarray(a, float).mean()
    b = np.asarray(b, float) - np.asarray(b, float).mean()
    d = np.sqrt((a * a).sum() * (b * b).sum())
    return (a * b).sum() / d if d else 0.0


def seams(img):
    a = np.asarray(img.convert("RGB"), float)
    lr = np.abs(a[:, 0] - a[:, -1]).mean() / max(np.abs(np.diff(a, axis=1)).mean(), 1e-6)
    tb = np.abs(a[0] - a[-1]).mean() / max(np.abs(np.diff(a, axis=0)).mean(), 1e-6)
    return lr, tb


def clean_alpha(img):
    a = np.asarray(img.convert("RGBA")).copy()
    opaque = a[..., 3] >= 128
    a[..., 3] = np.where(opaque, 255, 0)
    if opaque.any() and not opaque.all():
        # grow opaque colours outward into the transparent area, a few pixels per pass
        rgb = a[..., :3].astype(float)
        known = opaque.copy()
        for _ in range(16):
            if known.all():
                break
            acc = np.zeros_like(rgb)
            cnt = np.zeros(known.shape)
            for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
                k = np.roll(known, (dy, dx), (0, 1))
                acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
                cnt += k
            grow = ~known & (cnt > 0)
            rgb[grow] = acc[grow] / cnt[grow][:, None]
            known |= grow
        a[..., :3] = rgb.astype(np.uint8)
    return Image.fromarray(a, "RGBA")


def main():
    args = sys.argv[1:]
    only = []
    if "--only" in args:
        i = args.index("--only")
        only, args = args[i + 1:], args[:i]
    flip = "--flip" in args
    dry = "--dry-run" in args
    job = [a for a in args if not a.startswith("--")][0]
    out = os.path.join(TEX, "chatgpt", "jobs", job, "out")
    backup = os.path.join(TEX, "hd_backup_esrgan")
    os.makedirs(backup, exist_ok=True)
    installed = 0
    for path in sorted(glob.glob(os.path.join(out, "*.png"))):
        key = os.path.basename(path)[:-4]
        if len(key) != 16 or (only and key not in only):
            continue
        dumps = glob.glob(os.path.join(TEX, "dump", f"*_{key}.png"))
        if not dumps:
            print(f"{key}: no original in dump/, skipped")
            continue
        orig = Image.open(dumps[0]).convert("RGBA")
        res = Image.open(path).convert("RGBA")
        if flip:
            res = res.transpose(Image.FLIP_TOP_BOTTOM)
        want = (orig.width * 4, orig.height * 4)
        note = "" if res.size == want else f"  size {res.size}, resized to {want}"
        if res.size != want:
            res = res.resize(want, Image.LANCZOS)
        if np.asarray(orig)[..., 3].min() < 255 or np.asarray(res)[..., 3].min() < 255:
            res = clean_alpha(res)
        lr, tb = seams(res)
        # the generator drifts dark/pale variants toward mid tones; over ~20 levels shows as a patch
        bright = np.asarray(res.convert("L"), float).mean() - np.asarray(orig.convert("L"), float).mean()
        flag = "  BRIGHTNESS" if abs(bright) > 20 else ""
        print(f"{key}: layout {layout_corr(orig, res):.2f}  seams LR {lr:.2f} TB {tb:.2f}  brightness {bright:+.0f}{flag}{note}")
        if dry:
            continue
        hd = os.path.join(TEX, "hd", f"{key}.png")
        if os.path.exists(hd) and not os.path.exists(os.path.join(backup, f"{key}.png")):
            shutil.copy(hd, os.path.join(backup, f"{key}.png"))
        res.save(hd)
        installed += 1
    print(f"{job}: {installed} installed" + (" (dry run)" if dry else ""))


if __name__ == "__main__":
    main()
