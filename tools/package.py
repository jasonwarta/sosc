r"""Build the fix and bundle a release zip, optionally publishing it as a GitHub release.

usage: python tools/package.py <version> [--game <dir>] [--publish] [--notes <file>]
  e.g. python tools/package.py v1.0.0 --publish

The zip unpacks over a copy of the game's files:
  streets_fix.dll, dsound.dll         the fix and the DLL that loads it (fix/, built here)
  ddraw.dll, Shaders\                 cnc-ddraw (tools/cnc-ddraw/cnc-ddraw.zip; downloaded if missing)
  ddraw.ini, streets_fix.ini,         settings and the shortcut (dist/)
  Play (OpenGL).cmd, README.txt
  licenses\                           (dist/licenses)
  textures\hd\, textures\wide\        HD textures and widescreen HUD art (from the game folder)
  mods\moose\                         the moose: a patch for GEO\SIM3D1.MAX (made from the game
                                      folder's SIM3D1.MAX.orig) and the two sounds (SOUND\cow*.WAV)
The art comes from the development game folder (--game, default game/), not from git.
--publish tags the current commit as <version> and uploads the zip with gh (the GitHub CLI).
"""
import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))
import build  # noqa: E402

CNC_DDRAW_ZIP = os.path.join(ROOT, "tools", "cnc-ddraw", "cnc-ddraw.zip")
CNC_DDRAW_URL = "https://github.com/FunkyFr3sh/cnc-ddraw/releases/latest/download/cnc-ddraw.zip"
TEXT = (".ini", ".cmd", ".txt")  # written with Windows line endings


def cnc_ddraw():
    if not os.path.exists(CNC_DDRAW_ZIP):
        print("downloading cnc-ddraw:", CNC_DDRAW_URL)
        os.makedirs(os.path.dirname(CNC_DDRAW_ZIP), exist_ok=True)
        urllib.request.urlretrieve(CNC_DDRAW_URL, CNC_DDRAW_ZIP)
    return zipfile.ZipFile(CNC_DDRAW_ZIP)


def moose_patch(game):
    original = os.path.join(game, "GEO", "SIM3D1.MAX.orig")
    if not os.path.exists(original):
        original = os.path.join(game, "GEO", "SIM3D1.MAX")  # the moose was never installed here
    import make_moose
    make_moose.orient()
    return make_moose.make_patch(open(original, "rb").read())


def check_ini(game):
    """Warn about settings in the game folder's ini that the release ini doesn't have."""
    def keys(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            return {l.split("=", 1)[0].strip() for l in f if "=" in l and not l.lstrip().startswith(";")}
    live = os.path.join(game, "streets_fix.ini")
    if os.path.exists(live):
        missing = keys(live) - keys(os.path.join(ROOT, "dist", "streets_fix.ini"))
        if missing:
            print("WARNING: dist/streets_fix.ini lacks settings the game folder has:", ", ".join(sorted(missing)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("version")
    ap.add_argument("--game", default=os.path.join(ROOT, "game"))
    ap.add_argument("--publish", action="store_true")
    ap.add_argument("--notes", help="release notes file for --publish (default: generated from commits)")
    args = ap.parse_args()
    game = os.path.abspath(args.game)

    built = build.build()
    check_ini(game)
    name = f"StreetsOfSimCity-Fix-{args.version}"
    out_dir = os.path.join(ROOT, "out")
    os.makedirs(out_dir, exist_ok=True)
    zip_path = os.path.join(out_dir, name + ".zip")
    files = {}  # path in zip -> bytes

    for out in ("streets_fix.dll", "dsound.dll"):
        files[out] = open(built[out], "rb").read()
    with cnc_ddraw() as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            if info.filename == "ddraw.dll" or info.filename.startswith("Shaders/"):
                files[info.filename] = z.read(info)
    dist = os.path.join(ROOT, "dist")
    for dirpath, _, names in os.walk(dist):
        for n in names:
            path = os.path.join(dirpath, n)
            files[os.path.relpath(path, dist).replace(os.sep, "/")] = open(path, "rb").read()
    for sub in ("hd", "wide"):
        folder = os.path.join(game, "textures", sub)
        for n in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
            if n.lower().endswith(".png"):
                files[f"textures/{sub}/{n}"] = open(os.path.join(folder, n), "rb").read()
    files["mods/moose/SIM3D1.MAX.patch"] = moose_patch(game)
    for n in ("cowhit.WAV", "cowland.WAV"):
        path = os.path.join(game, "SOUND", n)
        if not os.path.exists(path + ".orig"):
            sys.exit(f"package: {path} isn't the moose sound (no .orig beside it); run tools/make_moose_sounds.py")
        files[f"mods/moose/{n}"] = open(path, "rb").read()

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for path in sorted(files):
            data = files[path]
            if path.lower().endswith(TEXT):
                data = data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")
            z.writestr(path, data)
    hd = sum(1 for p in files if p.startswith("textures/hd/"))
    print(f"{os.path.relpath(zip_path, ROOT)}: {len(files)} files ({hd} HD textures), "
          f"{os.path.getsize(zip_path) / 1e6:.1f} MB")

    if args.publish:
        if subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True, text=True).stdout.strip():
            sys.exit("package: commit your changes before publishing (the release tags the current commit)")
        cmd = ["gh", "release", "create", args.version, zip_path, "--title", f"Streets of SimCity fix {args.version}"]
        cmd += ["--notes-file", args.notes] if args.notes else ["--generate-notes"]
        subprocess.run(["git", "push"], cwd=ROOT, check=True)
        subprocess.run(cmd, cwd=ROOT, check=True)


if __name__ == "__main__":
    main()
