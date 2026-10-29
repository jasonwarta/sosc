"""Match each delivered texture's per-channel average to its original (keeps the raw file).

usage: python tools/match_tone.py <job-name> [key ...]

The image generator drifts dark and pale variants toward mid tones (or overshoots when told
to fix that). When the layout and texture are right, a per-channel gain toward the
original's averages fixes the tone without another round trip. Raw results go to
out/_raw/ first (once).
"""
import glob
import os
import shutil
import sys

import numpy as np
from PIL import Image

TEX = os.path.join(os.path.dirname(__file__), "..", "game", "textures")


def main():
    job, keys = sys.argv[1], sys.argv[2:]
    out = os.path.join(TEX, "chatgpt", "jobs", job, "out")
    os.makedirs(os.path.join(out, "_raw"), exist_ok=True)
    for path in sorted(glob.glob(os.path.join(out, "*.png"))):
        key = os.path.basename(path)[:-4]
        if len(key) != 16 or (keys and key not in keys):
            continue
        raw = os.path.join(out, "_raw", key + ".png")
        if not os.path.exists(raw):
            shutil.copy(path, raw)
        orig = np.asarray(Image.open(glob.glob(os.path.join(TEX, "dump", f"*_{key}.png"))[0]).convert("RGB"), float)
        im = Image.open(raw).convert("RGBA")
        a = np.asarray(im, float)
        gain = orig.reshape(-1, 3).mean(0) / np.maximum(a[..., :3].reshape(-1, 3).mean(0), 1)
        a[..., :3] = np.clip(a[..., :3] * gain, 0, 255)
        Image.fromarray(a.astype(np.uint8), "RGBA").save(path)
        print(f"{key}: gain r {gain[0]:.2f} g {gain[1]:.2f} b {gain[2]:.2f}")


if __name__ == "__main__":
    main()
