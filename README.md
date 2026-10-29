# Streets of SimCity on modern Windows

A fix for *Streets of SimCity* (Maxis, 1997) that makes it run properly on Windows 10/11 without
emulators: native OpenGL at any resolution and aspect ratio, a steady frame rate, no CD check,
a longer draw distance, HD textures, the smoke/fire particles the OpenGL renderer never had,
and a set of crash fixes. Everything is patched in memory at runtime; `Streets.exe` itself is
never modified.

## Playing

1. **Get the game.** None of its files are in this repository. *Streets of SimCity* is
   abandonware; MyAbandonware has it at
   [myabandonware.com/game/streets-of-simcity-df0](https://myabandonware.com/game/streets-of-simcity-df0).
   Take the **ISO version** (346 MB, the full CD): the fix is made and tested with it. The
   smaller "RIP" version reportedly gives Glide errors on modern PCs.
2. **Copy the game's files into a folder.** Double-click the ISO to mount it in Windows and
   copy everything on it into a folder such as `C:\Games\Streets`.
3. **Add the fix.** Download the zip from [Releases](../../releases) and unzip it into the same
   folder, overwriting when asked.
4. **Play.** Double-click `Play (OpenGL).cmd`. The zip's `README.txt` has the keys and settings.

## How it works

- `dsound.dll` sits in the game folder. Windows loads it as part of the game's startup (the game
  imports DirectSound); it passes every DirectSound call to the system DLL and loads
  `streets_fix.dll` just before the game's own code runs. No launcher, no code injection.
- `streets_fix.dll` installs the patches and hooks: frame pacing, OpenGL fixes, widescreen,
  draw distance, HD texture replacement, particles, the Apocalypse flash, and more. Each
  launch writes a detailed log to `logs\`.
- [cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) handles the game's DirectDraw menus.

[NOTES.md](NOTES.md) is the full engineering log: what was found in the game's code, every
fix and why, dead ends, and the dated history.

## Layout

| Path | What |
|---|---|
| `fix/` | C source of `streets_fix.dll`, `dsound.dll` (`dsound_proxy.c`) and the optional `streets_launch.exe`. |
| `fix/tests/` | Standalone tests for the trickier patches. |
| `dist/` | Files shipped as-is in the release: settings, the Play shortcut, the player README, licenses. |
| `tools/build.py` | Builds the DLLs (needs Zig 0.13). |
| `tools/package.py` | Builds and zips a release; `--publish` uploads it to GitHub Releases. |
| `tools/` (other) | Texture upscaling and ChatGPT texture-job helpers, the moose model and sound generators, MAX mesh and disassembly helpers. |
| `models/` | The moose model (Blender) and its texture. |

## Building

1. Get [Zig 0.13.0](https://ziglang.org/download/#release-0.13.0) for Windows and unpack it to
   `tools/zig-windows-x86_64-0.13.0/` (or set `ZIG` to its `zig.exe`).
2. `python tools/build.py` writes `fix/streets_fix.dll`, `fix/dsound.dll` and `fix/streets_launch.exe`.
3. Copy the two DLLs into a game folder that already has a release unpacked in it.

## Making a release

```
python tools/package.py v1.0.0             # out/StreetsOfSimCity-Fix-v1.0.0.zip
python tools/package.py v1.0.0 --publish   # also pushes, tags and uploads it with gh
```

The zip's art (HD textures, the widescreen HUD pieces, the moose sounds and the patch that
puts the moose model in `GEO\SIM3D1.MAX`) comes from a development game folder (`game/` by
default, `--game` to change it), which isn't in git. `--publish` needs a clean working tree
and the GitHub CLI (`gh auth login`).

## License

[MIT](LICENSE) for everything in this repository. *Streets of SimCity* itself belongs to its
rights holders and isn't covered. Releases also contain
[cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) (MIT).
