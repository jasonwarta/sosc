"""Build the fix: streets_fix.dll (all the patches), dsound.dll (loads it into the game, no
launcher needed) and streets_launch.exe (optional launcher), into fix/.

usage: python tools/build.py

Needs Zig 0.13 (its clang targets 32-bit Windows): tools/zig-windows-x86_64-0.13.0/zig.exe,
or set ZIG=path\\to\\zig.exe, or have zig on PATH.
fix/tests holds standalone tests for the trickier patches; each says how it's built and run.
"""
import os
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
FIX = os.path.join(ROOT, "fix")
TARGET = ["-target", "x86-windows-gnu", "-O2"]

DLL_SOURCES = ["streets_fix.c", "log.c", "opengl.c", "screenshot.c", "widescreen.c", "hdtex.c", "drawdist.c",
               "mods.c"]
OUTPUTS = {
    "streets_fix.dll": ["-shared", *DLL_SOURCES, "-luser32", "-lgdi32"],
    "dsound.dll": ["-shared", "dsound_proxy.c", "-luser32"],
    "streets_launch.exe": ["streets_launch.c", "-luser32"],
}
def zig():
    for z in (os.environ.get("ZIG"), os.path.join(ROOT, "tools", "zig-windows-x86_64-0.13.0", "zig.exe"),
              shutil.which("zig")):
        if z and os.path.exists(z):
            return z
    sys.exit("build: Zig not found (set ZIG or put zig-windows-x86_64-0.13.0 under tools/)")


def run(cmd):
    print(">", " ".join(os.path.basename(c) if i == 0 else c for i, c in enumerate(cmd)))
    subprocess.run(cmd, cwd=FIX, check=True)


def build():
    z = zig()
    for out, args in OUTPUTS.items():
        run([z, "cc", *TARGET, "-o", out, *args])
    return {out: os.path.join(FIX, out) for out in OUTPUTS}


if __name__ == "__main__":
    for path in build().values():
        print("built", os.path.relpath(path, ROOT))
