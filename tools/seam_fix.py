"""Make installed HD tiles meet their own repeat cleanly on one axis.

usage: python tools/seam_fix.py <seams.txt>      (lines: <key> tb|lr)

For tiles whose original repeats cleanly but whose repaint doesn't: the outer 1/16 of the
tile on both sides of the join fades ((1 - d/band)^2) to the mean of the two opposite edge
rows, so copies meet exactly without ghosting the far side's content. Works on
game/textures/hd/<key>.png in place (install_chatgpt_job.py keeps the first backup).
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

TEX = os.path.join(os.path.dirname(__file__), "..", "game", "textures")


def seam(a):
    lr = np.abs(a[:, 0] - a[:, -1]).mean() / max(np.abs(np.diff(a, axis=1)).mean(), 1e-6)
    tb = np.abs(a[0] - a[-1]).mean() / max(np.abs(np.diff(a, axis=0)).mean(), 1e-6)
    return lr, tb


def fix(path, axis):
    im = Image.open(path)
    a = np.asarray(im.convert("RGBA"), float)
    if axis == "lr":
        a = a.transpose(1, 0, 2)
    band = max(2, a.shape[0] // 16)
    edge = (a[0] + a[-1]) / 2
    out = a.copy()
    for y in range(band):
        w = (1 - y / band) ** 2
        out[y] = a[y] * (1 - w) + edge * w
        out[-1 - y] = a[-1 - y] * (1 - w) + edge * w
    if axis == "lr":
        out = out.transpose(1, 0, 2)
    Image.fromarray(out.round().astype(np.uint8), "RGBA").save(path)


def main():
    for line in open(sys.argv[1]):
        parts = line.split()
        if len(parts) < 2:
            continue
        key, axis = parts[0], parts[1].lower()
        hd = os.path.join(TEX, "hd", f"{key}.png")
        orig = np.asarray(Image.open(glob.glob(os.path.join(TEX, "dump", f"*_{key}.png"))[0]).convert("RGB"), float)
        small = lambda: np.asarray(Image.open(hd).convert("RGB").resize(orig.shape[1::-1], Image.BOX), float)
        before = seam(small())
        fix(hd, axis)
        after = seam(small())
        i = 1 if axis == "tb" else 0
        print(f"{key} {axis}: original {seam(orig)[i]:.2f}, before {before[i]:.2f}, after {after[i]:.2f}")


if __name__ == "__main__":
    main()
