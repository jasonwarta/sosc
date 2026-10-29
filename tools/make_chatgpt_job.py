"""Build a ChatGPT texture job folder from texture keys.

usage: python tools/make_chatgpt_job.py <job-name> <keys.txt> [--flip] [--style key ...]

keys.txt: one texture key (16 hex digits) per line, optionally followed by a label.
Creates game/textures/chatgpt/jobs/<job-name>/in/ with, per key, the original dump
(<key>__<label>.png) and a 4x nearest-neighbour layout guide (<key>__<label>__layout4x.png),
an OVERVIEW.png contact sheet of every texture in the job, and STYLE_REF_<key>.png copies
of already-accepted HD replacements (--style). REQUEST.md is written by hand.

The dumps are stored upside down relative to the game (trees hang trunk-up). --flip writes
the job images upright; flip the results back (install_chatgpt_job.py --flip) before use.
"""
import glob
import os
import shutil
import sys

from PIL import Image, ImageDraw

TEX = os.path.join(os.path.dirname(__file__), "..", "game", "textures")


def main():
    args = sys.argv[1:]
    style = []
    if "--style" in args:
        i = args.index("--style")
        style, args = args[i + 1:], args[:i]
    flip = "--flip" in args
    if flip:
        args.remove("--flip")
    job, keys_file = args
    root = os.path.join(TEX, "chatgpt", "jobs", job)
    os.makedirs(os.path.join(root, "in"), exist_ok=True)
    os.makedirs(os.path.join(root, "out"), exist_ok=True)
    items = []
    for line in open(keys_file):
        parts = line.split()
        if not parts:
            continue
        key, label = parts[0], (parts[1] if len(parts) > 1 else "tile")
        dumps = glob.glob(os.path.join(TEX, "dump", f"*_{key}.png"))
        if not dumps:
            print("no dump for", key)
            continue
        im = Image.open(dumps[0]).convert("RGBA")
        if flip:
            im = im.transpose(Image.FLIP_TOP_BOTTOM)
        im.save(os.path.join(root, "in", f"{key}__{label}.png"))
        im.resize((im.width * 4, im.height * 4), Image.NEAREST).save(
            os.path.join(root, "in", f"{key}__{label}__layout4x.png"))
        items.append((key, label, im))
    for key in style:
        shutil.copy(os.path.join(TEX, "hd", f"{key}.png"), os.path.join(root, "in", f"STYLE_REF_{key}.png"))
    cell, cols = 128, 8
    rows = (len(items) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * cell, rows * (cell + 14)), (255, 0, 255))
    draw = ImageDraw.Draw(sheet)
    for j, (key, label, im) in enumerate(items):
        x, y = (j % cols) * cell, (j // cols) * (cell + 14)
        bg = Image.new("RGBA", im.size, (255, 0, 255, 255))
        bg.alpha_composite(im)
        sheet.paste(bg.convert("RGB").resize((cell, cell), Image.NEAREST), (x, y))
        draw.text((x + 2, y + cell + 1), key, fill=(0, 0, 0))
    sheet.save(os.path.join(root, "in", "OVERVIEW.png"))
    print(f"{job}: {len(items)} textures, {len(style)} style refs")


if __name__ == "__main__":
    main()
