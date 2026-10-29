# Streets of SimCity on modern Windows: engineering notes

A running log of what we've learned about the game and the fixes. Newest lessons go in the
relevant section; dated entries at the bottom record how we got there.

## Layout

| Path | What |
|---|---|
| `game/` | Full copy of the 1997 disc, playable. Launch with `game/streets_launch.exe`. |
| `game/streets_fix.ini` | All fix settings (fps cap, WASD, renderer, fades, diagnostics). |
| `game/Play (OpenGL).cmd`, `game/Play (3dfx Glide).cmd` | Double-click launchers that pick the renderer (`-o2` / `-o1`), overriding `renderer=` in the ini. |
| `game/logs/session_*.log` | One log per session (last 20 kept). Read these first when anything breaks. |
| `game/screenshots/` | Ctrl+Shift+F10 captures. |
| `fix/` | Source: `streets_fix.c` (patches, hooks), `opengl.c` (OpenGL layer), `log.c`, `screenshot.c`, `streets_launch.c`. |
| `fix/tests/` | Standalone tests for code that touches the stack or calling conventions. |
| `analysis/streets.c` | Ghidra decompilation of Streets.exe (6,848 functions). |
| `original/` | Untouched binaries from the disc. |
| `tools/` | Ghidra, Zig (C compiler), cnc-ddraw, dgVoodoo2, crash catcher, disassembler. |

Build: `tools/zig-windows-x86_64-0.13.0/zig.exe cc -target x86-windows-gnu -O2 -shared -o streets_fix.dll streets_fix.c log.c opengl.c screenshot.c widescreen.c -luser32 -lgdi32` (from `fix/`).

Hotkeys (only when launched through `streets_launch.exe`): Ctrl+Shift+F10 screenshot,
Ctrl+Shift+F11 viewport scan, Ctrl+Shift+F12 emergency quit.

## How the game works (verified)

### Main loop and timing
- `FUN_005169b0` is WinMain's loop: drain `PeekMessageA`, then tick one frame (vtable +0xc).
  It loads `PeekMessageA` into `edi` once at `0x516ac0`, so an import-table hook installed
  late is never seen.
- (Superseded, see High-fps scoping below.) The simulation advances **per frame**. Uncapped it runs hundreds of fps: everything is too
  fast, AI glitches, and the fixed-point physics divide (`0x4c7960`, 64/32-bit `idiv`)
  overflows hundreds of times (the game catches those). Capping the loop at 30 fps fixes all
  of it.
- The game reads its CPU MHz at startup (`FUN_00556e30`, Intel's old routine) but only shows
  it on an info screen. Not a timing factor.

### High-fps scoping (2026-10-27)
- **The simulation is time-based, not per-frame** (this corrects the note above). The frame
  function `FUN_0044cb10` (app tick) does: input `FUN_004853f0`, then dt = (GetTickCount
  delta in ms) * 65536 / 1000 into `0x68ce84` (16.16 seconds, clamped 100..0x8000 = 1.5 ms to
  0.5 s), then the simulation update `FUN_004dcd10`, then (if `FUN_0049c540` view setup
  succeeds) the render `FUN_004d6730` + `FUN_0049c7a0`. 39 functions read dt: car physics
  (`FUN_004c8f80`: pos += vel*dt via `FUN_004ab0d9`), AI drivers (`FUN_004e8ea0`),
  pedestrians (`FUN_004f4cf0`..), the `0x59dxxx` engine (`FUN_0059dfb0`, 18 uses).
  `FUN_004dcd10` also keeps a smoothed dt (`0x68ce8c` = (7*avg + dt) / 8).
- Why uncapped ran fast: GetTickCount moves every ~15.6 ms. Above ~64 fps most frames measure 0
  ms, get clamped up to 1.5 ms, and hundreds of phantom 1.5 ms steps per second add up. At 30
  fps, 33 ms frames measured as 31 or 47 ms (judder).
- Fix: `precisetime=1` replaces the `call [GetTickCount]` at `0x44cb4b` with a QPC millisecond
  clock (`install_precise_time`).
- Frame-count-based logic found: the global frame counter `0x5e77bc` only paces mission checks
  (every 4 / 64 frames) and paused redraws (every 8); the sim tick counter `0x68ceac` is only
  used by the particle colour blink (`FUN_004ef3b0`, `& 7`). Both are harmless at 60.
- Not yet checked: camera follow smoothing, sprite animation frame stepping, sound triggers,
  anything else stepping by a constant per frame. The 60 fps test will show these.
- The log now reports "ms of work per frame" (time between our frame waits) next to fps.
  Under 16.7 ms means 60 fps fits.
- Plan: if 60 fps behaves, the interpolation rebuild isn't needed. Fall back to it only if
  per-frame logic misbehaves at 60.
- 2026-10-27: 60 fps test started (game/streets_fix.ini `fps=60`, `precisetime=1`). Waiting on
  the user's report. Check the session log's "ms of work per frame" afterwards.
- 60 fps result: "mostly feels fine" (no sound check; the player had no headphones on). Work per
  frame was 4-8 ms in light areas but 15-18 ms in dense ones (13k polygons, grid 61), so
  60 fps barely held and dropped to 55. That is far too slow for this hardware, so the cost is
  likely in our GL hooks or immediate mode, not the GPU.
- Frame profile (`frameprofile=1`): the call sites 0x44cb8d (sim FUN_004dcd10), 0x44cca4
  (render FUN_004d6730) and 0x44ccab (FUN_0049c7a0) are redirected to `__fastcall`
  pass-through wrappers. The targets take no stack arguments and end in a plain `ret`, so ecx
  and edx pass straight through. SwapBuffers is IAT-hooked. The "main loop" line now shows
  `(sim, render, post, swap)` ms. Now testing at fps=120.
- 120 fps profile, dense areas: sim 0.2 ms, render (FUN_004d6730) 10-12 ms, post 0.0,
  swap 0.2. All the cost is the 3D render call: geometry plus our per-vertex GL hooks in
  immediate mode. 70-90 fps downtown, 120 elsewhere. Next optimisation target: the render
  path (batch vertices instead of one GL call each through hooks).
- **Crash at 120 fps: EXCEPTION c0000095 (integer overflow) at 0x4ab114**, the idiv in the
  16.16 divide FUN_004ab0fb. Caller 0x59e222: speed = (pos [edi+0x529] - old [edi+0x541]) / dt.
  A warp or respawn jumped 5529 units in one frame, and dividing by 8 ms overflows. At 30 fps
  that needs a jump over ~1092 units, so the bug is original but much likelier at high fps. The
  game already writes 0 when dt == 0x8000 (a hitch). Fix: `divide_guard` VEH at 0x4ab114 and
  0x4ab131 (20.12 sibling). It returns 0 for the speed caller (return address 0x59e227) and
  saturates for any other caller, logging "divide guard: ...". Test:
  fix/tests/test_divide_guard.c. It maps the real addresses, so link it with
  `-Wl,--image-base=0x20000000`; VirtualAlloc reservations are 64 KB aligned.
- The "got stuck" at 60 fps was the car wedged in a corner between buildings (gameplay, not a
  hang). Probably original behaviour. Revisit only if it keeps happening at high fps, since
  collision response does integrate with dt.
- 20-minute map played to completion at 120 fps (session_20261027_171829). The divide guard
  never triggered.
- Two kinds of exception were caught by the game's own handlers during the render (no crash):
  * Write AV at 0x4b4379 (x5): `mov [edx+0x9bea10], cl`, the coverage-grid writer, whose grid
    is relocated to 0x03500100 by widescreen.c. Column = x >> 15 came out as 0x4cfa (19706),
    so a degenerate, huge projected x indexed 1.2 MB past the 8.9 KB grid. Unmapped here, but
    mapped memory would be silently corrupted. Wants a bounds clamp on the column/row (the
    sibling stores at 0x4b4320, 0x4b4350, 0x4b437f, 0x4b43xx have the same pattern).
  * Integer overflow at 0x4c7993 (x5 in one frame), stack 4c79e7 <- 4c6895 <- 4bd39f <-
    4b2649 <- 4a9c3e <- 4c7902 (render). Not yet disassembled.
  Probably the same bad polygon (vertex near the camera plane), so it's likely original
  behaviour, more exposed by the wider view.
- **Grid guard** (widescreen.c `grid_guard_install`, all renderers): the only call to
  FUN_004b3b00 (0x4ab91d, cdecl: polygon {count, verts}, flag; the result is the polygon's
  draw flag) goes through `coverage_guard`. It skips (returns 0) any polygon with a vertex
  outside x in [-7, w+8) px or y in [-7, 488) px (20.12 fixed point); those are the pixels
  whose tile exists in every grid. Each skip logs "grid guard: polygon with vertex ...
  skipped" (first 10). Test: fix/tests/test_grid_guard.c. If skips show up during normal
  play with visible holes, the bounds are too tight. The 0x4c7993 overflow shared the same
  render stack and may also stop now; watch for it.
- The vertex check alone did NOT stop the 0x4b4379 writes (4 more in session_174456, now at
  negative offsets, 0x0344xxxx). Cause: the per-tile-row edge interpolation. A nearly
  horizontal edge gets step = 0x8000/dy (FUN_004b3aa0 = a/b*4096), and the stepped x runs
  far outside the polygon. Fix: the four edge-x stores (0x4b3f0e, 0x4b3fd8 into 0x9c10f8;
  0x4b40fd, 0x4b41d0 into 0x9bd508; each a 7-byte `mov [idx*4+table], reg`) now call naked
  thunks (EDGE_THUNK in widescreen.c). These clamp a copy to the polygon's [min x 0x9c10e4,
  max x 0x9c10d4] and keep all registers. Count `ws_edge_clamps` is logged with the 30 s peak
  line. The test covers register preservation. Fixed-address tests occasionally fail to map
  (a random heap lands there); rerun them.
- The user won the 20-minute map (they never had before).
- End screen "YOU WON!" (shot_20261027_180014_625.png):
  * The dialog has a scrambled palette.
  * It's drawn unshifted at the left of the 4:3 area, with a black 640-ish block to its right.
  * The light-blue clear colour shows where the 3D view was.
  The log shows WM_DISPLAYCHANGE 640x480 8bpp: the game switches to DirectDraw (cnc-ddraw)
  for the dialog while the GL frame is still up. This is the same family as the Alt+F4 quit
  confirmation. Not fixed yet.
- Messages (top left) were missing or late early in the game and fine later. The package
  counter (top right) disappears while a new top-left notification shows; the user wants
  that fixed. Package list text: FUN_00454320 (string 0xc6ff "Package #%d: Time Left"),
  called from the HUD FUN_00453160 when [0x5e82e8]+0x330 is set. Messages time out 4000 ms
  after the obj+0x4029a timeGetTime stamp (time based, not fps). Screenshots of both states
  are needed.
- Screenshots 181016/181039/181046 settle it. The package counter is a spinning box plus a
  red number at top right. It is hidden while 3+ message lines show (pickup messages at top
  left) and back with 2. The user also says package messages didn't appear until they picked
  up an item (ammo, weapon). Overlay sizes seen: 560x17 (a text line, likely), 88x80, 40x43,
  80x32, 196x84, 488x240, 640x180, all via the blitter FUN_0051a900 (ret 0x51aaca).
  Instrumentation added: `log_overlay_callers` in opengl.c logs each overlay size's first
  game call chain ("opengl: overlay WxH drawn by ..."), to find who draws the box and what
  gates it.
- Overlay callers (session_182643):
  * 88x80, 560x17, 640x180, 68x133 and 360x113 come from dashboard panels via FUN_00450ec0
    (0x450efc).
  * 40x43 comes via 0x45abad/0x45abbc. That is FUN_0045a9b0 = **the radar**: it rotates the
    128x128 map into texture 0, and FUN_004ae7f0 draws the quad. FUN_0045af40 then draws
    package markers from the list at 0xa09600, so 40x43 is a radar element, not the counter.
  * The HUD gauge code in FUN_00453160 (fcos/fsin needles, 0x402ae.. / 0x40356..) is also
    not it.
  The spinning box + red number at top right is still unidentified. It is probably a 3D
  object or a 2D text/sprite path, not a glDrawPixels overlay. Next idea: a hotkey that dumps
  every draw call of one frame with return addresses, pressed with and without the counter
  visible.
- **Black world after the Esc menu**, confirmed again in session_182643: the new context
  after the menu round trip got the game's re-upload, but world textures stayed black. Root
  cause: FUN_0053a730 does wglMakeCurrent(0) + wglDeleteContext when entering 2D and
  wglCreateContext when returning. Fix: `persistentgl=1` (default). hook_wglDeleteContext
  skips the first context, and hook_wglCreateContext returns it again for the same DC, so
  textures survive. opengl_context_created() still runs each time (depth check, counters).
  Logged as "kept context reused" and "wglDeleteContext(...) skipped".
- **The persistent context did NOT fix the black world** (session_185431: still black after
  Esc, fixed by a gas station trip). The 384 world uploads did happen; they were converted
  with a black palette. **Root cause:** FUN_005262d0 (SetEntries from 0x52632f, not a fade
  routine) sets the whole palette to black before each fade-in, so hook_SetEntries copied
  black into PAL_BASE, the palette used for world textures. A new palette object (city load,
  gas station) re-seeds PAL_BASE from GetEntries. The texture uploads on that path happen
  between the seed and the black SetEntries, which is why those worked. Fix: a non-fade
  SetEntries that is all black (more than 16 entries) is treated as a fade, never copied into
  the base; logged as "palette: all-black SetEntries ... kept out". This is very likely also
  the old "black textures after the graphics dialog" bug. persistentgl stays on (harmless,
  avoids re-creation).
- Confirmed in session_190422: three menu round trips, 4 black SetEntries kept out, no
  exceptions. The user says "i think its good".
- **End screen / message boxes (still broken after the palette fix).** "YOU WON!" is string
  0xc496, shown by FUN_0043ef50 -> FUN_0043ab10(0x7e6, id) -> FUN_0043a740, a message box
  built from MBox*.bmp / button*.bmp (FUN_005268e0). In the screenshot, the coloured part of
  the box is exactly half of the box area and the other half is black: 8-bit bitmap rows
  written into a 16-bit surface. Entering GL 3D, FUN_0053a730 calls SetDisplayMode(640, 480,
  16) (push 0x10 at 0x53a764). Experiment: `ddraw8bpp=1` (install_ddraw_8bpp) patches it to
  8. **Works** (session_192730): the Alt+F4 quit box draws correctly, gameplay and textures are
  fine, no exceptions. "YOU WON!" uses the same box, so it is expected fixed (not yet seen).
- The user found the game's own quest/objective tracker shortcut, so the overlay idea is
  dropped.
- **Texture upscaling, step 1: census** (2026-10-27). `texturedump=1` (opengl.c
  texture_census) hashes the raw 8-bit indices of every COLOR_INDEX glTexImage2D (FNV-1a 64
  with w and h, so the key is palette-independent). Each new texture is saved once as
  game/textures/dump/<site>_<w>x<h>_<hash>.png (converted with the palette in use, keyed
  pixels alpha 0; up to 3000 per site). Per-site upload and distinct counts are logged every
  30 s ("texture census: site ..."). Known sites: 0x4b6da1 world, 0x4abb2d font, 0x4aed7d
  sprites (chroma-keyed), 0x4ae848 radar (every frame). Planned step 2: load
  game/textures/hd/<hash>.png (via GDI+ or WIC) in place of the upload, building mipmaps,
  and check whether city palettes differ for the same indices.
- Census results (session_193311, a few minutes of driving):
  * 004b6da1: 344 world textures, all 32x32 (walls, windows, ground, signs).
  * 004b6959: 61 textures, 128-256 (roads, trees, explosions, car skins, train, jet, people).
  * 004b6b34: 5 textures.
  * 004aed7d: 43 sprites, 128x128 (explosions, pedestrian frames), re-uploaded every frame
    (90k uploads).
  * 004abb2d: font, 256x256, 69 distinct, **re-uploaded for every text draw (1.3M uploads)**.
    That is likely a large share of the render time: a cache would help the 120 fps goal.
  * 004ae848: radar, about 1000 distinct (dynamic).
- **Texture upscaling, step 2: loader** (hdtex.c + opengl.c, `hdtextures=1`). hdtex_init
  indexes game/textures/hd/<16 hex>.png. Decoding uses GDI+ (flat API via LoadLibrary), and
  rows are flipped to match the dump's bottom-up PNG (test: fix/tests/test_hdtex.c). The game
  uses fixed texture names (no glGenTextures) and uploads into whatever is bound. On a level-0
  colour-index upload (radar excluded) whose hash has a replacement:
  * the HD image goes into our own object (HD_NAME_BASE 0x40000000 + i) with
    GL_GENERATE_MIPMAP, trilinear filtering, 8x anisotropy and repeat wrap;
  * the game's bound name is redirected to it in hook_glBindTexture;
  * the game's upload is skipped.
  An upload without a replacement into a redirected name removes the redirect.
  hook_glTexParameteri ignores MIN_FILTER on HD objects. opengl_context_created(fresh) drops
  the objects on a real new context.
- Upscaler: tools/realesrgan (realesrgan-ncnn-vulkan 20220424, Vulkan). The pipeline is
  tools/upscale_textures.py: fill transparent pixels from neighbours, pad (wrap for world,
  edge otherwise), x4, crop, threshold alpha, cap at 512. The auto-permission classifier
  blocks running the downloaded exe; the user must run it or allow it.
- First upscale run (user ran it, 2026-10-27): 450 textures, realesrgan-x4plus, 36 MB in
  game/textures/hd. Buildings and windows come out sharp; water and ground lose their dither
  and look smoother. First in-game test launched ("hd textures: 450 replacements").
- In-game: "super crisp". Bug: capital letters got noise boxes. The main font sheet comes via
  0x4b6b34 (key 084b725b208fbb99, 256x256) and is keyed near-black at draw time; the upscaled
  copy has soft edges. Its HD file was removed; it is in SKIP_KEYS in upscale_textures.py, and
  key_bound_text_texture now logs "... is a font sheet" instead of keying an HD object.
- Water "animation too fast" (user, with HD + 120 fps). World polygons pick their texture as
  material table 0x629f18[mat*0x14] + frame offset (poly+0x28 = material<<16 | frame;
  FUN_004ac030 binds 0xa0f810[(base + 0x9bd4a8)*6]). No per-frame water re-uploads (the census
  shows world uploads only on context changes) and no palette cycling, so water animates by
  switching the frame offset. Where that offset advances is not yet found: per-frame or
  random selection would run 4x faster at 120 fps, and HD frames differ more from each other
  than the dithered originals. A/B requested: hdtextures=0 vs fps=30.
- A/B result: at fps=30 with HD on, water is "closer to the base game", so the water speed is
  frame-rate bound. The frame offset advances per rendered frame somewhere; still to be found
  and tied to dt. game/streets_fix.ini is at fps=30 for now.
- Water animation hunt: not found statically. Ruled out:
  * face material frame (poly+0x28, static from the PLAN chunk);
  * the every-4th-frame block in the frame function (it calls FUN_004a20c0, which returns 0);
  * the terrain builders FUN_004bbd50/4bc2d4/4bc8bf (no modulo/phase);
  * FUN_004c9a50 (camera turn).
  Terrain draws through FUN_004abc00 (bind ret 0x4abc7a, glBegin ret 0x4abc82) with the
  texture index straight from the gridfaces record [0]. Probe: `texprobe=1`
  (opengl_frame_tick from the main loop). Each 5 s it logs texture names bound in 4-60 of the
  last 64 frames, with a usage bitmap and bind site. Plan: if water cycles through names,
  remap them in hook_glBindTexture to a time-based frame, without needing to find the game
  code.
- **Probe result** (parked at water, 30 fps): terrain bind site 0x4abc7a cycles names 2,3,4,5,6
  and 7..11 in lockstep, each held for exactly 2 game frames (`.##........##...`). The water
  animation is frame-based: 15 steps/s at 30 fps, 60 at 120. Fix (`wateranimms=67`):
  hook_glBindTexture remaps terrain binds of names 2-11 to group base + (QPC ms / 67) % 5.
  water_frame() turns a group on only after 32+ frames with exactly one of its names bound
  per frame, 3+ distinct names seen and never two in one frame. A group that ever shows two
  names in one frame is "not an animation, left alone". Logged as "water: texture group ...".
  ini fps is back to 120 for the test.
- v1 of the water detector turned itself off: one frame with two names bound (probably the
  mirror pass, or the game stepping between two terrain passes) marked the group "not an
  animation" for good. v2: on when clean >= 32 and mixed*4 <= clean, counts halved every 256
  frames, at most 20 on/off log lines.
- v2 retiming works (the probe shows each water name held 8 frames at 120 fps = 67 ms), but the
  user still says it looks wrong: "textures go up and down as well as the surface being
  animated". Motion probe (texprobe): parked, the water texcoords and screen positions are
  constant (no UV scroll, no vertex waves); the wave motion is drawn into the 5 frames. At 120
  fps the game itself held each name 4 frames, so its step is not purely per-frame either.
  Likely culprit: the HD water frames are upscaled independently, so the AI detail differs
  frame to frame and the animation boils. `hdwater=0` (default) skips HD for uploads into
  names 2-11. Awaiting the user's verdict.
- User: the HD water is fine; the texture motion is just too fast. 67 ms matches 30 fps, which
  the user called only "closer" to the original (1997 hardware likely ran slower). Game ini
  now has wateranimms=133 and hdwater=1; tuning by eye.
- **In-car dashboard in widescreen** (extendhud=1): the cockpit dashboards are 640-wide
  overlays (640x180 bug, 640x192 others; wheel 488x240/392x230, A-pillar 100x289 separate).
  When a 640-wide overlay is drawn at raster x 0, extend_full_width_overlay draws column 12
  (and w-13) as a 1-pixel strip zoomed to ws_hud_off wide at canvas x 0 and 640+off. The
  needles and radar are separate, so the dash itself can't be spread apart.
  v1 (one column stretched) looked bad: the grain became horizontal streaks and the right side
  came out black with artifacts. v2 tiles a 32-column block from 12 px in (mirrored, so no
  seams); each side strip also covers the 12 rounded-corner columns. The roof bar, mirror
  frame and A-pillar are separate overlays and are not extended yet.
- **Wide dash art** (user made it with ChatGPT outpainting): game/textures/wide/640x180.png
  (Beetle; the original centre is pixel-exact) and 640x192.png (wood dash; the user let it
  repaint the centre to fix the curvature, used as-is). Loader: opengl_set_wide_dir indexes
  <w>x<h>.png and <16 hex>.png once. draw_wide_overlay crops the canvas-wide window around the
  centred original and draws it in place of the 640-wide overlay; without a file it falls back
  to the tiled edge extension. Sources: game/textures/widen_src.
- Other cockpit pieces, identified from the dumps (560x17 was a roof bar, not a message line):
  560x17/540x17 roof bars, 88x80 Beetle roof corner, 100x289 wood car A-pillar, 68x133 and
  360x113 third-person dash parts. cockpit_piece(): the 88x80 and 100x289 at raster x 0 are
  pinned to canvas x 0; bars (h <= 24, 400 <= w < 640) are zoomed to w + 2*hud_off from canvas
  x = raster_x. Each position is logged once ("cockpit piece ...").
- Session 202325: 14.5 ms of work per frame at 30 fps with only ~3 ms of render. Something
  else costs ~10 ms; check whether that is new (overlay conversions?).
- Grid guard edge clamps run into the thousands per session (2600 after ~2.5 min). Normal
  polygons overshoot by up to a tile at shallow edges, so clamping to the polygon's x range
  only tightens their coverage, which is harmless. No 0x4b4379 faults in session_182643.
- End screen after finishing the map (and the menu after Alt+F4/Esc-quit) shows broken
  graphics. Need a screenshot. Likely the same family as the black textures after the graphics
  dialog (2D menu over the GL/DirectDraw switch).
- User wants a quest/objective tracker. The game doesn't have one; it would mean finding the
  mission state in memory and drawing an overlay. Logged as a feature idea.

### Renderers
- `-o0` software, `-o1` Glide (3dfx), `-o2` OpenGL. `DAT_0062ac88` is the active renderer
  (0/1/2). `FUN_0053a730` switches between 2D menus (DirectDraw) and 3D.
- All renderers work in **640x480 screen coordinates**; the game does its own projection.
- The OpenGL path was evidently never finished (the readme never mentions it). Every bug
  below was in the original code, hidden by 1997 drivers that didn't follow the spec.

### Projection (for widescreen)
- `FUN_004a9ba5(rect)` loads a viewport record: x, y, width, height, half-res flag.
- `FUN_004ab326` derives the projection: centre = width/2, height/2;
  **focal = half-width x 0x1bb6/4096 (= 1.732, i.e. 60 deg horizontal FOV)**. So FOV is tied
  to width: widening as-is would crop top/bottom. Plan: derive focal from height
  (`half_height x 2.309`, identical at 4:3).
- `FUN_004aef10` builds frustum planes from the same values.
- Render quality modes: `DAT_005e7850` 0x10 full-res, 0x20 half-res (Ctrl+Q).
- Rear-view mirror is a second viewport at (448,11)-(630,75), drawn mirrored.
- View records (x, y, w, h, half-res flag, pitch w, pitch h), runtime values: main `0x5e7878`
  = 640x480, half-res `0x5e7858` = 320x240, mirror `0x5e7928`. `FUN_004a9ba5` is cdecl and
  starts `push ebp / mov ebp,esp / mov eax,[ebp+8] / pushal`.
- Polygons reach GL as `glVertex4f(x*w, y*w, 0, w)` in screen space: x = proj_x * `0x6293f8`
  (double, negated for the mirror) + `0x629410` (double). The mirror sets `0x629410` = 631.0
  as an immediate (`0x4adcac`, high dword `0x4083b800`) and y offset 9.0.
- Ortho is set once in `FUN_0053a910` (renderer switch): `gluOrtho2D(0,640,480,0)` on the
  MODELVIEW matrix (the projection is identity).
- 2D glBegin sites: text `0x4ae5fc`, radar quad `0x4ae852`, radar lines `0x4aec63`. All
  other glBegin sites (`0x4abc82`, `0x4acf2a`, `0x4ad780`) are 3D.
- **Widescreen (`widescreen.c`, 2026-10-27):** coordinate space ws_canvas_w x 480 (480 x
  aspect, multiple of 8; 1144 at 3440x1440). Detour of FUN_004a9ba5 widens the main and
  half-res records (OpenGL only). A call at `0x4ab379` re-derives focal from the half-height
  (x 9459/4096) for widened views only, which keeps the mirror's own FOV. gluOrtho2D right
  edge becomes the canvas width. Overlays move by ws_hud_off = (canvas-640)/2: glRasterPos2i,
  the three 2D glBegin sites (modelview translate), the fill-rect detour, and the mirror x
  immediate. ini `widescreen=auto|off|W:H`; auto = primary monitor.
- **Coverage grids (widescreen crashes, 2026-10-27):** `FUN_004b3b00` (culling) keeps
  three 8x8-tile grids, column-major, sized for 640x480: `0x9bea10` and `0x9bfd70` (61 rows
  x 81 cols) and `0x9bd618` (62 x 82, referenced as base and base+62). It never clamps x; it
  relies on the view width. At 1144 wide it overflowed into its own loop variables
  (`0x9c10c0`.., `0x9be9f4`..) and then wrote into .rsrc (first crash `0x4b46be`, second
  `0x4b4558` after only two grids were moved). Fix: heap grids sized canvas/8 + the
  original's spare columns + 2, 21 relocation-table references in `0x4b3a00..0x4b4800`
  rewritten, clear-loop column count immediate at `0x4b3ad8`. Row arrays `0x9c10f8` and
  `0x9bd508` are per y-tile and fine.
- **Bounds sweep for widescreen (2026-10-27).** Functions reading the view width:
  `0x4bc8bf`, `0x4c6314` and `0x4c71c4` clip polygons to it (bounded); `0x4b5017` clamps
  to it; `0x4b2ec2`, `0x4bd47d`, `0x4be46a` and `0x4becee` are software rasterizer and debug
  text (software framebuffer `0x5e783c` and row table `0x628444`, unused in OpenGL). Code
  constants 640/639/320/640-fixed: Glide clip window, radar line clip and glow clip at 639
  (Glide-only), menu bitmaps, and a false positive in `0x4bb7a0` (ID codes). Per-frame
  lists: polygons `0x6f8610` (0x71 bytes, cap 15000, flushed when full) and vertices
  `0x896348` (0x14 bytes, cap 60000, **never checked**, 4 per polygon at the cap). Peaks are
  logged every 30 s (`widescreen_frame`) to see whether a guard is needed. First run at
  1144 wide: 3234 polygons and 10608 vertices peak, so no guard is needed.
- **Sprite 639 clamp (widescreen, 2026-10-27).** The sweep missed this one because it uses
  neither the width globals nor a 640 immediate: it uses the double `639.0` at `0x5c4aa8`.
  `FUN_004ad140` (OpenGL billboard sprites: trees, bushes, pedestrians; flag-4 polygons, one
  screen-space quad) clamps the right edge to 639 and sets u = 1 - overshoot/width. A sprite
  right of 639 becomes a quad stretched back to x=639 with u negative, so the texture repeats.
  Symptoms: rows of copies of pedestrians, grids of trees, and a hard vertical cut at canvas
  x=640. Fix: `fcom`/`fsub` at `0x4ad87d`/`0x4ad892` point at our own double (canvas-1), and
  the float immediate 639.0 stored at `0x4ad898` is replaced. Other refs to `0x5c4aa8`:
  `0x4adb7f` (Glide fog quad), `0x4aeb26`/`0x4aeba8` (radar, 640 space, correct as is).
  Lesson: also search `.rdata` for the edge constants as doubles and floats (639.0, 479.0,
  640.0), and list the references to each.
- **Draw distance.** Saved in `streets.cfg` (MIFF records: tag, size incl. 8-byte header, data) as
  the `GOFV` record (slider 0..24). The game rewrites the file only on a clean exit, so in-game
  changes were lost after crashes. `drawdistance=max` (ini) patches the record at every launch
  (`apply_draw_distance`). Other prefs: `0xffffff03` = four graphics toggles (all 1 already),
  `0xffffff02` first dword = 3D resolution radio (0 always low, 1 low/high, 2 always high;
  `FUN_0049c5xx` uses the half view 0x5e7858 for 0, and for 1 when busy). `resolution=high`
  forces 2 at launch (`apply_game_settings`, which also handles draw distance). Dialog labels
  (screenshot 13:12): Building textures, Ground textures, Sky, Near-Fog-Far slider, and the
  three resolution radios. The world is a square window of map tiles around the camera:
  `DAT_00a0f7c0` (odd, 13..61, hard cap `0x3d` in `FUN_004c56d0` and the pool sizing in
  `FUN_004c5bf0`, 0x103e60 bytes for grid arrays sized n^2). Options slider 0..24 maps through
  `FUN_004dd2d0`: n = s*2 + 13. In-game keys DIK 0x33/0x34 (`,`/`.`) step it by 2. The far
  clip and fog come from `FUN_004d16a0((n/2 - 3) * 0x400000)`, with GL fog start/end at 13/15
  x that / 65536. Going past 61 needs the cap, the pool and every n-sized array (an audit
  like the coverage grids). The log now shows the current n.
- **Software texel reads past the texture (gas station crash, 2026-10-27).** The car preview at
  gas stations and garages uses the software mapper even under `-o2`. The mapper keeps u+v*w as
  20.12 in ebx and converts with `shr esi,0xc`, so a slightly negative offset at a polygon edge
  reads about 1 MB past the texture (crash at `0x4b7cbc`, ebx=fff9f78a). Windows 95 usually had
  that memory mapped. Original-game bug, not widescreen. Fix: a vectored handler (`texel_guard`)
  for read faults at `0x4b7cbc` (`mov ah,[esi]`) and `0x4b8e55` (`mov al,[esi]`) zeroes the
  byte and skips 2 bytes. Unrelated: `0x4f9486` is a stale pointer in the map object lists
  (`0xb42fb0`), still open.
- **Sprite depth test: tried and reverted (2026-10-27).** World polygons wrote depth (`GL_ALWAYS`,
  z from w); sprites were depth-tested. The user reported fog far too dense, and graphics broken
  after Esc to the menu and back. Causes: (1) fixed-function fog uses eye-space z, so feeding
  real z into glVertex4f (the game passes 0) changed fog for everything; (2) the cached
  depth-test state didn't survive the menu's context recreation (wglCreateContext). Reverted
  completely. If retried: give fog its own coordinate (`GL_FOG_COORDINATE_SOURCE`) or put z in
  the projection matrix rather than the modelview, and reset cached GL state on
  wglCreateContext/MakeCurrent. Far-tree flicker stays an original-game limitation for now.
- **Sprite depth test v2 (2026-10-27, awaiting test).** Same depth scheme. Fog: the game's
  glVertex4f z = 0 means fixed-function fog (eye z) never showed in OpenGL at all, so fog now
  reads `GL_FOG_COORD_SRC = GL_FOG_COORD` with glFogCoordf(0) (from wglGetProcAddress, per
  context), which is identical to the original. Contexts: the game calls wglCreateContext at
  every menu round trip (5-7 per session). `opengl_context_created()` resets the cache, and
  every glViewport (frame start) disables the depth test and clears depth. The depth buffer is
  32 bits here. `spritedepth=0` turns it off. Side note: the game's GL fog has never worked.
  Real fog could hide the far pop-in, as Glide does.
- **Black world textures after the graphics dialog (2026-10-27, open).** Esc, then the graphics
  dialog, then OK: world textures (32x32, uploaded once by `FUN_004b6b50` from ret `0x4b6da1`)
  draw black; per-frame textures (car, cones) are fine. Esc and back alone is fine. The game
  makes a new GL context per menu round trip, and a new context has no textures. Likely the
  dialog path doesn't re-run `FUN_004b6b50`. Logging added: 'previous context had N texture
  uploads (M world)' at each wglCreateContext. Candidate fixes: keep one persistent context
  (wglCreateContext returns the first, wglDeleteContext is a no-op for it), or re-run the
  upload. The user prefers just starting in high res (done), so this is lower priority.
- **Polygon budget at max draw distance:** 14899 / 15000 polygons and 48266 / 60000 vertices
  peak at grid 61 (widescreen 1144). Raising the draw distance past 61 also needs a bigger
  polygon list (`0x6f8610`, 0x71 bytes each) and vertex pool.
- The game's DirectDraw palette pointer lives at `0x690714` (created when a city loads,
  not during menus). Runtime changes go through `SetEntries` (vtable slot 6). Callers seen:
  `FUN_005262d0` (ret `0x52632f`) sets the real palette; fades `FUN_00525b00` (`0x525c6d`)
  and `FUN_00525fc0` (`0x5261f7`, `0x52629c`); `FUN_0046e310` (`0x46e554`) is the
  Apocalypse white flash (steps entries toward white).
- The fix keeps two palettes: *current* (everything, used for per-frame uploads: HUD,
  radar, font) and *base* (excludes fades/flash, used for world textures uploaded once).
  The hook is installed lazily each frame once `0x690714` is non-null (hooking
  `IDirectDraw::CreatePalette` didn't work, likely because the game uses a newer interface).
- The OpenGL path uploads only the *default* palette from the exe's resources
  (`FUN_0053a580`, resource 123) via `glPixelMapfv`, so anything relying on runtime palette
  changes (radar blips blinking red) was wrong.

### 2D overlays (HUD)
- `FUN_0051a900` blits images. Image object fields: +0x08 width, +0x0c height, +0x30
  has-transparency flag, +0x48 transparent colour index, +0x50 pixels. In its OpenGL branch
  the object is in `ebx` at the `glDrawPixels` call (return `0x51aaca`, row path `0x51ab35`).
- Text: `FUN_004ae310(x, y, char)` draws one glyph as a **16x16** textured quad from the font
  texture (uploaded by `FUN_004aecd0`, return `0x4aed7d`), but characters advance only 9 px,
  so each quad overlaps the previous letter. Glide hides glyph backgrounds with chroma key
  against its default key, black, **compared in RGB565** (anything with r<8, g<4, b<8 is
  keyed). Keying only exact black left near-black backgrounds as boxes over letters.
- Radar: `FUN_004ae7f0` uploads a 128x128 map into **texture object 0** every frame
  (return `0x4ae848`) and draws one quad; heading lines via `FUN_004aea80`. Enemy blips are
  pixels inside that texture in a palette colour the game animates, not separate primitives.
- `FUN_004aed80(color 0x00RRGGBB, x0, y0, x1, y1)` (cdecl) fills a rectangle. **Only its
  Glide branch exists**; in OpenGL it did nothing, so the rear-view mirror's sky kept the
  forward view. Detoured to a scissored clear in OpenGL (`opengl.c`, `install_fill_rect`).
  Mirror sky colour `0x90d0ef` (day) / `0x02090f` (night).
- Sweep of functions with a Glide branch but no OpenGL branch (2026-10-26): only
  `FUN_004aed80` (fill rect, fixed), `FUN_004adc60` (mirror clip window; not needed, the
  game clips mirror polygons itself) and `FUN_004ade20` (light flares/glow sprites:
  **missing in OpenGL**, to do).
- When patching code from `opengl_install`, guard the read (`IsBadReadPtr`): the standalone
  tests call it too and the game's code isn't mapped there.

### Other
- Install path = exe folder. Registry only used for joystick settings.
- CD check `FUN_005322a0`: accepts a location only if its drive has **zero free bytes** (a
  CD) and `smk\intro.smk` is > 5 MB. Patched at `0x5323c7` so the game folder passes.
- Blocking fades: music volume `FUN_0051d350` (2-2.5 s per transition), palette fade
  `FUN_00525fc0`. Capped by `fademax`.
- Session destructor `FUN_00411e50` uses the player global `0x6925d4` without a null check
  (crash on teardown). Guarded at `0x4120dd`.
- Errors are shown in the game's own UI (not MessageBox), then `ExitProcess(0)`: crashes
  look like clean exits unless exceptions are logged.

### Font and shadows (OpenGL)
- The text glyphs actually use a **256x256 font sheet** uploaded by `FUN_004abad0` (return
  `0x4abb2d`), not the 128x128 texture from `FUN_004aecd0` (likely sprites). Found by
  probing the bound texture during a glyph draw and reading it back with `glGetTexImage`.
  Some rows of the sheet have opaque black backgrounds: Glide's black chroma key hides them.
- `FUN_004ac030` draws 3D polygons. Type `0xb` (material +0x10) = translucent shadow/glow.
  Glide: material +0x28 < 0x30 → shadow, blend (ZERO, SRC_ALPHA) with constant colour
  `DAT_0062acbc` (multiplies the ground darker); >= 0x30 → glow, blend (SRC_ALPHA, ONE),
  alpha `(0x538 - v*0x18) << 24`, colour `DAT_0062acb8`. The OpenGL branch instead uses
  (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) with `glColor4b(0,0,0,0)` / `(1,1,1,0)`: alpha 0,
  invisible. Not yet fixed.
- The 256x256 font texture used by text (GL texture 475 in one session) is filled before
  any upload through `0x4abb2d` is seen, so keying by upload site missed it. Now keyed
  where used: the first glyph drawn with a texture reads it back, keys RGB565-black and
  re-uploads; later uploads into that texture are keyed too.
- **Colour byte order bug** in the OpenGL branches of `FUN_004ae700` (text colour) and
  `FUN_004aea80` (radar lines): a 0x00RRGGBB colour unpacked with shifts 24/16/8 gives
  (0, R, G). Pink quest text drew green, the green clock blue. Patched the six shift
  immediates (`0x4ae77d/83/86`, `0x4aec49/4f/52`) to 16/8/0.
- Flat-polygon census: bright-blue strip colours are flat polygons from `0x4acf2a` with
  colours like (36,44,152), (53,62,169), (86,98,202): full-brightness base colours. Glide
  darkens them with per-vertex shading; the OpenGL branch overwrites the shaded colour.
  Shadow polygons arrive as blend on (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) colour (0,0,0,0).
- **Shadows and the bright-blue patches** (under trees and along buildings), from the
  disassembly of `FUN_004ac030`'s OpenGL polygon branch (Ghidra's decompile was misleading
  here): per vertex it sets the shaded colour from the lighting table `0x5e7894`
  (`glColor3b`, ret `0x4acf6f`), then overwrites it with `[0x9bd440]` (ret `0x4ad045`), a
  stale material colour in OpenGL (Glide updates it). Shadow type 0xb: `glColor4b(0,0,0,0)`
  ret `0x4ad02b`, glow `(1,1,1,0)` ret `0x4ad021`, blend set at ret `0x4acf22`. Glide
  constants: shadow `0x62acbc` = `0xb7000000` (dst x 183/255), glow colour `0x62acb8` =
  `0x7f7f7f`. Fixed by call-site-aware `glColor` and `glBlendFunc` hooks.
  Many shadow polygons never call `glColor` at all (Glide used a constant colour), so they
  inherited the last colour set anywhere (green radar lines, black, blue) and flickered.
  The shadow/glow colour is now set in the `glBlendFunc` hook, before `glBegin`.
  **Lesson: Glide constant-colour state has no OpenGL equivalent; anything relying on it
  must have its colour set explicitly, or it picks up stray state.**
- Second source of stray colours: polygons with material flag 0x20 whose texture is
  switched off (`glDisable(GL_TEXTURE_2D)` branch) set no colour in OpenGL. Glide colours
  them brightness x (0.95, 0.90, 0.85) (constants `0x5c4a78/7c/80`; brightness 0-255 per
  polygon in `0x9bb2fc`). Set in a `glShadeModel(GL_FLAT)` hook (ret `0x4acedc`), which
  runs just before their `glBegin`. The flat-polygon census flags any remaining batch drawn
  with no colour of its own as `[current colour]`.
- Exception `0x6ba` (RPC server unavailable) inside KERNELBASE appears occasionally; it's
  Windows-internal (first-chance, handled), not the game.
- Unexplained access violation seen once at `0x4f9486` (read of a wild pointer, stack via
  `0x46f521`, `0x4dccff`); the game continued. Watch for repeats.
- Idea (user): replace the font sheet (and later other textures) with our own images by
  substituting uploads in the `glTexImage2D` hook; same mechanism as AI upscaling.
- Flat-coloured polygons: the OpenGL branch sets the per-vertex shaded colour (flag 0x40,
  table `DAT_005e7894`) and then overwrites it with `DAT_009bd440`; Glide uses the shaded
  colour. Suspected cause of bright-blue strips at building bases (dark shadows in Glide).

### Reference screenshots (`reference/`)
Original 1997 captures found online (old-games.com, gamefabrique.com):
- Text has **no background or outline**; it is coloured: quest text pink or dark red, clock
  green or red. The font texture is white glyphs tinted by the text colour (Glide colour
  combiner); the OpenGL port left `GL_REPLACE`, so text came out white. Fixed with
  `GL_MODULATE` around text quads.
- Radar blips are magenta/pink.
- The Apocalypse mission starts at **night**: dark sky, buildings darkened via the palette.

## Lessons
- **Widening a 1997 engine: look for screen-sized static arrays.** Anything sized for 640
  overflows silently into neighbouring globals; the crash shows up far away. The exe's
  relocation table lists every absolute reference, so a static array can be moved by
  rewriting those entries.
- **Read the log line by line before concluding.** A grep for one phrase missed lines that
  were there, and I briefly claimed the patch never ran.

- **Log everything, always on.** Most bugs were found from the session log alone: fault
  addresses, the game's own error text, stall samples, renderer switches.
- **The game hides its crashes.** It catches exceptions, shows its own message and exits
  normally. Log first-chance exceptions from a vectored handler.
- **Watch for other hooks.** cnc-ddraw also hooks `PeekMessageA` and `GetClientRect` (it
  reports 640x480). Patch the call site, or use calls it doesn't intercept
  (`GetWindowInfo`).
- **1997 OpenGL code vs modern drivers.** Things old drivers tolerated and modern ones
  don't, all in this game:
  - colour-index `glDrawPixels`/textures through pixel maps (wrong colours): convert to RGBA
  - fog applies to `glDrawPixels` (HUD tinted sky blue)
  - `glBlendFunc(SRC_ALPHA, ZERO)` turns transparent pixels black: use alpha test
  - **`glColor3b/4b` take signed bytes**; the game passes 0-255, so every component >127
    became 0 (text alpha 0 = black boxes, bright colours black)
  - texture object 0 with a mipmap min-filter and no mipmaps is incomplete and draws
    untextured (radar)
  - game code paths with only a Glide branch (fill rect) silently do nothing in OpenGL;
    when something is "missing" in OpenGL, look for `if (DAT_0062ac88 == 1)` without a
    matching `== 2`
- **Test anything that touches the stack before it reaches the game.** A naked-asm thunk
  that captured `ebx` mishandled the return address and crashed the game on every load.
  `fix/tests/test_drawpixels_thunk.c` now runs it standalone.
- **Diagnose from data, not guesses.** The fastest wins came from dumping what the game
  actually sends: image dumps (`dumpimages=1`), primitive census, stall watchdog, viewport
  scan.
- **Don't trust a 1-pixel conclusion.** A "text strip" turned out to be the windshield
  frame; the real text was textured quads.
- Escape-sequence trap in this workspace: bash heredocs that feed Python which writes C can
  turn `\n` and `\\` into real newlines/backslashes. Use the editor for C string literals.

- cnc-ddraw's renderer must match the game's 3D renderer: `opengl` with `-o2` (to share
  the window with the game's OpenGL), anything but OpenGL with Glide (dgVoodoo's Direct3D 11
  stays black otherwise). `streets_launch.exe` rewrites `renderer=` in `[Streets]` of
  `ddraw.ini` on every launch.
- A new build only takes effect when the game is next launched; the DLL can't be replaced
  while the game runs. Check the session log's start time against the DLL's timestamp.

## Dead ends
- Emulators / CPU throttling: rejected (tried before this project).
- dgVoodoo2's DirectDraw for menus: drew 640x480 unscaled in the corner, mouse mapped to the
  scaled layout. Kept as `game/ddraw.dll.dgvoodoo`.
- Keeping cnc-ddraw (Direct3D 9) and dgVoodoo (Direct3D 11) on one window: renderer
  switches cost ~1 s and the menu went black. Moving to the OpenGL renderer instead.

## Status
- Working: 30 fps cap, WASD, no-CD, teardown crash guard, dialog centering, fade caps,
  emergency quit, screenshots, full logging, OpenGL at native resolution (centred 4:3),
  HUD colours and transparency, radar map with blinking enemy blips, mailbox/cone colours,
  rear-view mirror sky.
- Text confirmed correct in OpenGL (2026-10-26): pink quest text, green clock, no boxes.
  Took four fixes: signed `glColor` bytes, `GL_MODULATE` tinting, colour byte order, and
  RGB565 black keying of the real font texture (18,140 pixels in texture 475).
- Strips under buildings and trees confirmed fixed (2026-10-27). These are untextured flag-0x20
  polygons, now coloured brightness x (0.95,0.90,0.85) from the `glShadeModel` hook at ret
  `0x4acedc`. The log shows the warm grays, e.g. (227,215,203). Shadows (type 0xb) are drawn
  with the right colour.
- Widescreen runs at 3440x1440 (2026-10-27) after two coverage-grid crash fixes. The sprite
  repeat and cut bug is fixed and awaiting a test. The user found max graphics plus max draw
  distance (61) good enough. Gas station visit worked with the texel guard build (12:31; the guard didn't fire that run,
  since the fault is intermittent). Trees and pedestrians
  confirmed fixed up close; sprite depth test v2 built (fog coordinate + context reset), awaiting test; draw distance forced to max at launch.
  Verified 12:59: `draw distance: slider 6 -> 24`, peaks 11112 polygons (no list flush). The user
  still sees slight far flicker; probably painter's sort (original). A/B (13:04, `widescreen=off`): the user
  confirmed the flicker is in the original 4:3 too, just more visible wide. Ini set back to `auto`.
- Still unchecked: glow sprites (`FUN_004ade20`, Glide-only branch).
- Next: Apocalypse flash in OpenGL, widescreen with `width`/`height` settings, then AI
  texture upscaling.

## Log
- 2026-10-26: project start; timing, crash, WASD, CD check, logging, fades, OpenGL bring-up.
- wateranimms=133 was still "sickeningly fast". The log shows the retime working: each water name
  is held 16 frames at 120 fps. So the step rate was never the problem. A contact sheet of the 5
  water frames (names 2-6, keys 32d87fcd.., 57ab7020.., e03ea54c.., f2311067.., 961076b1..)
  shows why. The originals are near-identical dither noise; consecutive frames match best
  shifted 1 texel left, but they are not clean shifts, the noise is fresh each frame. The
  Real-ESRGAN versions are five unrelated pictures (calm, choppy ripples, very choppy,
  scratched lines, flat), so flipping between them strobes at any speed. Upscaling the frames
  one by one can't animate.
- Fix: with hdwater=1, terrain binds of a retimed water group use HD frame WATER_HD_FRAME (2,
  the rippled one) and scroll it along s through the GL texture matrix by real time
  (waterdrift texels/s of the 32-texel tile, default 4; wraps each tile so the float stays
  precise). Any other bind resets the texture matrix to identity (set_texture_scroll). Water
  texcoords are in tile units (probe st 0.0156 = half a texel of 32), so an s offset of 1.0 is
  one tile. hdwater=0 keeps the retimed flipbook of the originals.
- Drift at 4 texels/s (one tile per 8 s) was "better but still fast"; the user asked for 1, so the
  game ini now has waterdrift=1 (one tile per 32 s).
- Video analysis (user's Snipping Tool clip, 3438x1438 at 30 fps; frames pulled with OpenCV,
  which needs Windows-style paths from Python). With the camera parked, the near water in the
  cove drifts smoothly (about 0.1-1 px/frame, the HD drift). The open water beyond a tile
  boundary changes a lot every frame: phase correlation on a far patch gives vertical jumps of
  +-15..30 px, back and forth. The far shoreline rows hold still, so the land edges are not
  moving. This is the "up and down" the user meant. It's not the flipbook: the probe shows
  only names 4 and 9 bound, so both groups are in drift mode. Next probe, "water totals": with
  texprobe=1, every 5 s for 23 frames it logs the per-frame sums of s, t, x/w, y/w and w over
  all water vertices, to tell whether the game moves the far water's texcoords or its
  vertices per frame.
- First water-totals run logged only zeros: my bug, the totals were cleared before the log line.
  Fixed (log, then clear) and relaunched.
- Water totals (parked, 120 fps): ~1064 water verts. sum(y/w) alternates between two values
  every frame (about +-290 over all verts), and sum(w) alternates with it (+-1200). So the water
  vertices flip between two heights/depths each frame: a 2-frame bob, 60 Hz at 120 fps. On top
  of that, the game's own s/t creep slowly (about 0.03-0.06 per frame summed), and the
  alternation phase slips now and then. Next probe: land totals over the same frames (is it the
  camera shaking?) and a per-vertex comparison of consecutive frames ("water verts: N, M moved
  >0.5 px", worst vertex before/after).
- Per-vertex result (parked): the land totals are constant (y/w and w both steady), so the
  camera isn't shaking. Only the water moves. The water vertices split into two sets, about
  205 and 318 verts, and each set moves on alternate frames. A moving vertex travels steadily,
  e.g. #1196 y 115.2 -> 111.1 -> 107.0 -> 102.9 -> 98.8 (about 4 px per update, every 2nd
  frame). These are real geometric waves that advance per frame, half the water per frame,
  so at 120 fps they run 4x as fast as at 30. The game's s/t barely change (0-2 verts per
  frame). The wave code has not been found statically (no "wave" names; the parity checks in
  FUN_004d1960 are tile building). Next: memdiff probe (texprobe) snapshots .data/.bss
  (0x5e6000-0xc53000) on 6 probe frames and logs the 4 KB pages with the most changed
  dwords, to find the wave height array and then its writer.
- Memdiff of .data/.bss (parked): only sparse changes per frame. 0x5e77b0 is the frame counter
  (FUN_0044c970, +1 per frame); a few dwords in 0x686000-0x68b000 (car/camera state?); and
  0xa9e000-0xaa0000 (the terrain render list at 0xa9e3b0) toggles values by +-1. No dense
  wave array there. The world grid vertices are on the heap: FUN_004bbd50 transforms
  *(int **)0xa0f788 ("gridvert", allocated as 0xa0bc38 * 12 bytes, x y z ints) through the
  matrix at 0xa0f6d0. New probe: diff gridvert between frames (logs "gridvert: N of M
  changed", per component), then arm a hardware write watchpoint (DR0 via a helper thread
  that suspends the main thread; VEH logs the eip and stack for 8 hits) on the first changed
  coordinate, to name the wave code.
- **Found the wave code.** The gridvert diff shows y (height, 16.16) of water vertices changing
  by exactly 0x10000 per write, on 93-190 verts. The write watchpoint caught eip 0x4dba65
  (FUN_004db9f0, which rebuilds gridvert y from the height map 0xb42d80, ushort per cell)
  called from FUN_004d74c0 (ret 0x4d76f7). It runs twice per frame: from the sim path (stack
  0x4d518b, 0x44cb92) and from the post-render path (0x4567df ... 0x49c76e, 0x44ccb0). The
  wave stepper is FUN_004dba90, called at 0x4d75f4 when DAT_005e77ec == 1. It adds dt
  (0x68ce84) to an accumulator 0x68cdb4 and compares it against threshold 0x68cdb8, but
  resets the threshold to 0 on every step, so it steps on every call. Each step adds
  profile[idx & 31] (tables 0x68ce00 / 0x68cdc0, alternating even/odd columns over a 4-phase
  cycle 0x68cdbc) to every height-map cell whose type (0xb32d80) is 5 (water). It returns 1
  so FUN_004db9f0 rebuilds the heights. So the waves step twice per frame: 60/s at 30 fps,
  240/s at 120.
- Fix (streets_fix.c, wave_step): redirect the call at 0x4d75f4 to a wrapper that calls the
  original only once every wavems of QPC time (default 33 = 30 steps/s, as at 15 fps on
  1997 hardware), returning 0 otherwise (no rebuild). Resyncs after a gap > 250 ms instead
  of catching up. ini wavems (0 = original). Logged in "patches: ... wave_timing=ok". Game
  ini texprobe back to 0 (the gridvert watchpoint only arms under texprobe).
- User verdict on wavems=33: "much much better". The water speed issue is solved. Current game
  ini: wavems=33, hdwater=1, waterdrift=1, wateranimms=133, texprobe=0. The gridvert and
  water-totals probes stay in the code under texprobe for future animation hunts.
- User asked for waterdrift back to 4 (they liked that texture speed; the complaint was always the waves). Game ini: waterdrift=4.
- ChatGPT handoff (user has a ChatGPT Work Project opened on game/textures). The protocol is in
  game/textures/chatgpt/README.md: one folder per job, chatgpt/jobs/<name>/{REQUEST.md, in/,
  out/}. ChatGPT writes only to out/ (exact names and sizes, PNG, alpha kept) and leaves
  out/NOTES.md. Template: chatgpt/jobs/_TEMPLATE.md. The user relays: "read chatgpt/README.md,
  then do job <name>".
- Texture families for re-painting (from contact sheets of the dump):
  * 004b6da1 32x32 (344): world tiles: building facade units, terrain (grass, dirt, sand,
    water and the blend tiles between them), signs (STOP, H, P, OCEAN GAS).
  * 004b6959 (61, mostly 128x128): roads (straight, crossings, diagonal), car UV atlases
    (station wagon, sedan, the train, the jet), trees (with alpha), explosions, wheels, a cow.
  * 004aed7d 128x128 (44): explosion and fire sprites, ragdoll people.
  * 004ae848 (radar) and 004abb2d (font): skip.
- First ChatGPT job: chatgpt/jobs/pilot-realism. Nine textures (two facades, a concrete wall,
  grass, grass+dirt, dirt, road straight, road crossing, the station wagon atlas). Asks for a
  fresh realistic paint that keeps the UV layout (4x nearest-neighbour layout guides at the
  output size), is seamlessly tileable for ground and walls, avoids baked lighting, and has
  medium-contrast detail. Outputs are out/<key>.png at 4x (128 or 512). Plan for checking:
  downscale and compare, a 3x3 tiling test, then in game (copy into hd/ after backing up the
  Real-ESRGAN version).
- pilot-realism results (ChatGPT, built-in image generator; it left NOTES.md, QA sheets and
  generation.json). Measured: layout correlation (blurred luminance, result box-downscaled to
  the original size) 0.95-0.99 for the facades, dirt, roads and car, 0.82 for concrete. Seam
  ratio (wrap-edge diff / mean adjacent diff, about 1 = seamless): 0.7-1.7 for the ground,
  concrete and crossing; road LR 2.8 (it meets the sidewalk, fine). White facade TB 7.06 vs
  the original's 0.84: needs a fix (floors stack vertically). Facades' LR seams are high in
  the originals too (5.6-7.7).
- My mistake: the grass labels were swapped (247407485aa2a66b has the dirt patch, 7a192f403c70be20
  is plain). Installed swapped, with the patched result rolled (8, 48) px to put the patch
  bottom-left (best correlation 0.50 at roll 2,12 in 32-px units). Labels fixed in REQUEST.md.
- Installed all nine into game/textures/hd; the Real-ESRGAN versions are in
  game/textures/hd_backup_esrgan/ (copy back to revert).
- In-game verdict on the pilot (user): "omg this already looks so much better". The direction is
  confirmed.
- Next job: chatgpt/jobs/roads-2. The other 7 road textures from 004b6959 (689e19933ca1b51a
  intersection, 7f7e7ade3999bc82 curb-left edge, 842c8b682fc36516 dark bridge road,
  8e0c44a98f3d3ca1 diagonal with sidewalk, a2b2a35b184e1d6e highway diagonal,
  c19866943efc9829 highway straight, cd2082d7f7b0cf4c highway crossing), with the accepted
  pilot roads as style references (in/STYLE_REF_*), plus the fix for the white facade
  054d6b28dd6b5f53 (vertical seam, drift, reflections). Remaining families after roads: the
  344 32x32 world tiles (facades, terrain blends, signs), the car atlases, then trees and
  sprites with alpha.
- Queued ground jobs (tools/make_chatgpt_job.py builds a job folder from a keys file: dumps,
  4x layout guides, an OVERVIEW.png contact sheet, STYLE_REF copies of accepted HD files).
  Terrain tiles came from the 004b6da1 set, filtered by a "no full-length straight edge"
  heuristic plus an eyeball pass, then grouped by colour: ground-grass (36, grass, blends, two
  gravel), ground-dirt (42, dirt, sand, rocky slopes), ground-snow (9). Water and shore tiles
  are excluded on purpose (animated water names 2-11 and the shore blends; revisit later).
  The REQUESTs stress that blend boundaries stay at the same pixels and that grass and dirt
  are consistent across the set, so neighbours meet. chatgpt/QUEUE.md lists the pending jobs
  so the user can just say "do the pending jobs".
- Sprites: the dumps are stored upside down relative to the game (trees hang trunk-up, people
  head-down; the dump writes GL row 0 at the PNG top). tools/make_chatgpt_job.py --flip gives
  ChatGPT upright images, and tools/install_chatgpt_job.py --flip turns results back. Open
  question: the non-sprite pilot textures (facades, the car atlas) were painted as seen in the
  dump, so if the game shows them flipped, the gravity-dependent details may be upside down.
  Check in game (reflections and sills on the white facade, the station wagon's sides).
- tools/install_chatgpt_job.py: per out/<key>.png it checks/resizes to 4x, reports layout
  correlation and wrap seams, thresholds alpha at 50% with colour bleed into transparent
  pixels (for sprites), backs up the old HD file to hd_backup_esrgan/ (first one kept) and
  installs to hd/. --dry-run, --flip, --only.
- Queued sprite jobs: sprites-trees (6), sprites-people (16-frame pedestrian walk cycle,
  painted as one sheet for consistency), sprites-fire (36 explosion/fireball/smoke/debris
  frames: 10 from 004b6959 plus 26 from 004aed7d).
- roads-2 results: layout 0.95-0.99 on all 7 roads; seams match each original's character
  (e.g. 7f7e curb edge LR 4.2 vs the original's 7.4; the 8e0c diagonal 2.9/2.5 vs 5.1/4.4).
  Facade fix 054d: layout 0.94, reflections toned down, TB seam 4.56 (was 7.06, original
  0.84), still not perfect. All 8 installed.
- roads-2 NOTES from ChatGPT: the facade's upper glazing is still a few px high and the floor joint is an intended red-to-grey architectural joint; the crossing marks are cleaner than the original. It didn't update QUEUE.md (it was given the job by name); I marked roads-2 done.
- **Draw distance past 61 (fix/drawdist.c, ini worldtiles, game ini 91).** The user is fine
  with 60 fps ("go for broke"). Audit of everything sized for the 61-tile window:
  * FUN_004c56d0(n) rejects n > 0x3d (cmp ecx,0x3d at 0x4c5708; imm8 at 0x4c570a) and carves
    the grid arrays out of a pool. FUN_004c5bf0 presets the n=61 sizes (0x4c5bf0..0x4c5c18)
    and allocates pool = 33(n+1)^2 + 252 n^2 bytes (push 0x103e60 at 0x4c5c2c; the formula
    gives exactly 0x103e60 at 61).
  * Static [61][61] gridfaces row-pointer table at 0xa0bca0, with globals right after it.
    Filled at 0x4c5968 (mov esi,0xa0bca0) / 0x4c5994 (add esi,0xf4). Read twice in
    FUN_004d6730 with the stride 61 built from lea arithmetic (0x4d682d, 31 bytes;
    0x4d68e5, 28 bytes). Moved to the heap, and the reads go through thunks face_row_eax /
    face_row_ecx (row ebp, column [esp+0x14]).
  * Polygon list 0x6f8610 (0x71 B; base refs 0x4ab47e/5bd/6f9/813/948) and vertex list
    0x896348 (0x14 B; refs 0x4ab4bb/4e5/60c/63b/74a/76c), the cap 15000 at
    0x4ab453/593/6d2. The list isn't sorted: it's drawn in submission order (far to near)
    after a reverse coverage-cull pass, so a flush when full is only a perf cost. Moved to
    heap buffers of 15000 * area * 1.5 polygons and 6 vertices per polygon.
  * All other window-sized arrays are heap (the pool), and the other grid functions index
    map-wide 128/256 arrays. The window must stay < 128 (map coordinates are masked);
    capped at 125.
  * The 7 calls to FUN_004c56d0 (0x4d762d, 0x4db2df, 0x4dd0f5/150/1b3/1fa, 0x4dd268) go
    through grid_resize, which maps the game's max 61 to worldtiles. The far clip and fog
    scale by themselves.
  * Every site's bytes are verified in a dry-run pass before any patching (no half-patched
    state). New tool: tools/xref_range.py (instruction-level xrefs per Ghidra function,
    without the false positives of a raw byte scan).
  First launch at 91: "draw distance: extended, max window 91 tiles, polygon list 50073,
  vertex list 300438", all patches ok. Awaiting the in-game test.
- First in-game test at 91: the ground (terrain and roads) didn't render; only some buildings
  did (they were in the right place, so the camera and projection were fine). FPS was fine
  (120, render 4-5 ms, peak 6573 polygons). Cause: two static 560000-byte terrain buffers that
  the terrain pass fills through write pointers 0x62afcc / 0x62afd0 with no capacity check:
  0xa9e3b0 (16-byte records; refs 0x4b229d 26aa 2734 2835 285d 2c2b 2c53 2dd9, 0x4bd0b7) and
  0xa15820 (refs 0x4b22a2 26af 2739 29aa 29ba 2afc 2b0c). They're followed by function
  pointers (0xa9e3a0) and other globals. Moved to heap buffers of 560000 * area * 1.5;
  peak use is logged with the per-frame peaks. My first audit missed them because they're
  sized by drawn terrain, not by the grid globals. Ruled out on the way: the visible-cell
  list 0x9c5050 (room for 16384 = 128^2), the frustum edge lists 0x9c4398 / 0x9e5050, and
  the pool (the allocator FUN_00559f80 chains new blocks when full, so the pool size is not
  a limit). Also seen: 5 integer overflows at 0x4bc6ab (near-plane clip, x*focal/near in
  FUN_004bc2d4), all in one frame, survived via the game's SEH. Watch.
- Second test at 91: still no ground ("almost feels like it's only rendering beyond"). The new
  log shows the terrain buffers barely used (3696 B of a), so they were not the cause (the
  relocation stays as a safety margin). Polygon peaks are similar to 61 (6.5k vs 7-11k).
  The fault logger only logs the first 5 exceptions per code (under_limit), so the
  near-clip overflows at 0x4bc6ab / 0x4bc61f may happen every frame. Theory: with the window
  45 tiles to each side, triangles crossing the camera plane far off to the side overflow
  x*focal/near, and the game's SEH drops the frame's terrain. Added projection_guard
  (drawdist.c): a VEH over all 16 idivs in FUN_004bc2d4 / FUN_004bc547 / FUN_004bc8bf
  that saturates the quotient to +-0x20000000 and logs a running count every 30 s
  ("projection guard: N overflows saturated so far"). Installed even at 61. Also still on
  the table: a hard-coded half-window offset (30 tiles / 0x7800000 in FUN_004e8410,
  FUN_004f4e20, FUN_004fb1d0), which the user also finds plausible.
- ground-grass results: 36 delivered. The blend tiles keep their boundaries (layout
  0.93-0.99) and the dirt matches the pilot dirt. 14 dark scrubland tiles (dark brown-olive
  rough ground fading into grass) came back as bright lawn (layout -0.36..0.73) and one has
  yellow streaks (cc4cdb997c3fc2d0). Installed the 22 good ones (install_chatgpt_job --only);
  the 14 are queued as ground-grass-fix with the previous attempts as reference. Lesson for
  the remaining requests: name the dark/rough variants explicitly, since the generator
  normalises to "nice lawn".
- Projection guard result at 91: 3001 overflows in about 8 s (constant, every frame): the theory
  is confirmed. Polygon peaks went from 6.5k to 10.5k and the far city, the bridge and hills
  render. But the plain ground tiles (grass, dirt, water: FUN_004abc00 terrain) are still
  missing over a wide area, while roads and buildings (object path) draw. Also new access
  violations at 0x4b3f3c (coverage rasterizer, from 0x4ab922: read 0x0376fff0, negative
  index) after the saturation, and fps 35-46 (render 20-27 ms) with the wide view. The user
  ended the session with the panic hotkey (not a crash). The saturation is lowered to
  +-0x08000000 (2048 px in 16.16). Discriminating test: worldtiles=63. Ground fine means
  magnitude/overflow problems at large n; ground missing means something structural still
  assumes 61.
- 63-tile test: ground still missing (plain cells such as open water and grass), while cells
  with objects (roads, sidewalks, buildings) draw, and there were no projection overflows. So
  it's structural, not magnitude. Clue: an access violation at 0x4b3f3c (coverage pass
  FUN_004b3b00, called from 0x4ab91d), reading 0x033ffff0, which is 16 bytes before one of my
  VirtualAlloc'd lists. The original arrays had globals on both sides, so reading a little
  before the start was harmless. On a bare allocation it faults, the game's SEH abandons the
  reverse coverage pass, and every polygon not yet flagged stays undrawn. Fix: alloc_padded
  gives every relocated buffer a zeroed 4 KB page of slack on both sides. Game ini still at
  worldtiles=63 for the retest.
- User: the diagonal road's edges don't line up in game. Probably one diagonal texture is
  mirrored for both directions, so its left/right edges must match when flipped. Add to a
  roads fix job (8e0c44a98f3d3ca1 diagonal with sidewalk, a2b2a35b184e1d6e highway diagonal).
- Diagnostic worldtiles=-61 (every relocation installed, window kept at 61): the ground renders
  perfectly. So the relocations are correct, and the missing ground at 63+ comes from
  something else that assumes n <= 61. Ruled out by reading: FUN_004d16a0 (far clip,
  bands 0xa0bc3c/40 only sort object polygons for the software path), FUN_004bc2d4 (per-face
  projection, all n-scaled), FUN_004d78e0 (per-cell frustum cull). Next experiment built and
  deployed (game ini worldtiles=63): "ground:" log lines (visible cells, cells with objects,
  culled, faces, polygons, window origin, camera tile), once a second for 8 s after any window
  size change. Plan: stand still and flip 61 <-> 63 with the in-game , / . keys, then
  compare which stage loses the ground.
- The user went to bed at ~00:30 on 2026-10-28 and asked me to keep the ChatGPT queue going;
  they told ChatGPT to watch QUEUE.md. A session cron (job 4cfceef7, :17 and :57) reviews and
  installs finished jobs and queues new ones. Queued roads-diagonal-fix (#10): in game the
  diagonal tile's curb doesn't meet the straight road's curb (screenshot crop in in/). It
  asks for exact border pixels, the same curb width as 7f7e, and asphalt without streaks
  (the tile is probably mirrored for both directions).
- User: the diagonal misalignment is where one diagonal tile meets another, mirrored diagonal
  tile (they were driving parallel to the diagonal). Measured: the original 8e0c joins its own
  rot180 copy (seam ratio right 0.90 / below 0.33) and its transpose/rot90 copies (0.92 /
  0.59); ours breaks those (1.55 / 0.70, 1.37, 1.01). The original also has a small curb jog
  in the rot180-right placement, so it's not certain which placement the game uses. Added
  in/SEAM_TEST_<key>.png (four placements, original over current) and an update section to
  the roads-diagonal-fix REQUEST asking for continuous lines in all four. Ask the user for a
  screenshot of the diagonal-diagonal seam to pin down the transform.
- Check-in ~01:00 2026-10-28: ground-dirt delivered 42/42 (no NOTES.md yet). The rock
  ledges and gullies are kept (layout mostly 0.88-0.99; low scores on near-uniform tiles
  mean nothing). New check, mean brightness vs original: 12 light sandy tiles came back
  22-46 levels darker (blotches next to pale neighbours). Installed the other 30; queued
  ground-dirt-fix (#11) for the 12. Pitfall: Python print on Windows emits CRLF, so the \r
  broke a grep -F filter in bash and 11 of the 12 dark tiles were installed anyway; restored
  from hd_backup_esrgan/. Strip \r (tr -d '\r') when feeding Python output to shell filters.
  Brightness drift is now a standard review check (the generator normalises both dark
  scrub and pale sand toward mid tones).
- Check-in ~01:40: ground-snow done (9/9 with NOTES). Snow/rock boundaries kept (layout 0.98 on
  the blends; the plain snow tiles score low because they're near-uniform), brightness within
  8 levels, and the big seam numbers are the snow/rock edges (as in the originals). The rock
  lost the original's dark crevice shading (flat brown). Acceptable at mountain distances, but
  a candidate for a later polish pass. All 9 installed. Queue: 6 jobs pending (trees, people,
  fire, grass-fix, diagonal-fix, dirt-fix), so no new job yet.
- Check-in ~02:20: sprites-trees done (6, with NOTES and alpha validation). Real foliage cards,
  trunk bases on the original pixels, transparent gaps in the crowns. The greens drifted
  lighter and more yellow-olive (+12..+18 brightness; the "dark green" 16db tree lost most of
  its darkness), and tree variety narrowed (1334/16db/f33e look alike; f33e kept few red
  flowers). The bare tree's branches are thin (may drop out under the 50% alpha cut at a
  distance). Installed all 6 with --flip (alpha thresholded with colour bleed). Candidate
  polish job: restore each tree's distinct hue/darkness.
- Check-in ~03:00: sprites-people done. ChatGPT painted one 4x4 sheet and cut it, so it's the same
  man in all 16 frames (consistent outfit and build; it removed an invented bag/sunglasses).
  But the foot registration was off: feet 28-45 px (of 512) lower in 12 frames and the figure
  about 5% short, which would make him bob as he walks. Corrected mechanically: per frame, scale
  the figure's bounding box to the original frame's height (x0.98-1.09), put its bottom on the
  original frame's foot row and its centre on the original's centre. That reproduces the
  original's own per-frame rise and fall exactly. Raw results kept in out/_raw/. Installed
  all 16 with --flip (layout now 0.7-0.85).
- Remaining 004b6da1 tiles after the ground jobs: 251. Facades (most), water/shore (still
  excluded), ~11 snow-rock tiles the snow job missed, signs/icons (STOP, P, H, F, hot dog,
  lightning, checkers, newspaper, traffic light, cone, OCEAN GAS...) for a later signs job, and
  some flat colour tiles (roofs?). Queued buildings-1 (#12): 47 clear facades, no flip (in the
  dump doors stand on the bottom edge, so world tiles are upright as dumped; only sprites are
  stored upside down). The REQUEST stresses tileability, positions and brightness/colour per
  tile, naming dark and pale variants. Scratch index: rest.txt (index, key, water fraction).
- Check-in ~03:40: sprites-fire 36/36 (no NOTES yet). Silhouettes kept (layout 0.68-0.98, most
  0.85+), brightness within +15, one consistent look across all frames (white-hot cores,
  orange flame, sooty edges); the white flash frames gained fire detail. Installed all 36
  with --flip. Pending: ground-grass-fix, roads-diagonal-fix, ground-dirt-fix, buildings-1
  (47), which is enough work for now.
- Check-in ~04:20: ground-grass-fix done. The dark scrub is back with the right boundaries
  (layout 0.73-0.96), but ChatGPT overshot: everything 12-24 levels too dark, cc4c 40. New
  tool tools/match_tone.py: per-channel gain to the original's RGB averages (raw kept in
  out/_raw/). After it, the tiles read like the originals (pale dirt patch back). All 14
  installed.
- roads-diagonal-fix: ChatGPT's NOTES admit it couldn't make the borders exact, and the seam
  ratios got worse (8e0c rot180 R 1.55 -> 1.80). Not installed. Instead, a border-band blend
  of the installed HD with a bicubic upscale of the original (24 px band, weight
  (1-d/24)^1.5 from each edge): seams now match or beat the original (8e0c rot180 R0.78
  B0.30, transpose R0.80, rot90 R0.50; a2b2 0.84-1.14). Installed both (backups in
  hd_backup_esrgan). A faint line along the tile edges comes from the original's own border
  pixels. Lesson: pixel-exact border constraints are better done in code than by the
  generator, so the same band blend could be offered for the facades if their joins show.
- Check-in ~05:00: ground-dirt-fix done (12). Brightness within +-9 and layout 0.93-0.99; a few
  leaned pink, so match_tone aligned the colour. All 12 installed, so the dirt set is complete.
  buildings-1 is in progress (20/47); it's the only pending job, so the facades come next.
- Queued buildings-2 (#13, 53 facades, same rules as buildings-1, dark variants named). keys_bld3.txt (54 more facades) is ready in the scratchpad for buildings-3.
- Check-in ~05:40: buildings-1 47/47. Colours and brightness held (all within +-16, so naming
  the dark/pale variants worked), features in place (layout 0.73-0.99; the 3 low scores are
  plain stucco/panel tiles). All 47 installed. Queued buildings-3 (#14, the last 54
  facades) after buildings-2.
- Check-in ~05:00: buildings-1 NOTES arrived (no files revised after my install). ChatGPT says
  the borders aren't mathematically seamless. Measured at the original's 32 px scale (an HD
  tile's smoother neighbour differences inflate the ratio at 128): 18/47 are worse than the
  original, but most of those originals aren't seamless either (doors, corner pieces). For the
  6 whose original joins cleanly (TB: 1f36 3a76 5330 4e5c 384e; LR: 0ffc), an edge-average
  blend: the outer 8 px on both sides fade ((1-y/8)^2) to the mean of the two opposite edge
  rows, so the copies meet exactly with no ghosting. Seams are now at the original's level
  (e.g. 3a76 TB 2.13 -> 0.66, orig 0.14; 0ffc LR 3.64 -> 1.19, orig 1.16). Installed. Apply
  the same check to buildings-2/3.
- Check-in ~06:20: buildings-2 done (53, with NOTES). No brightness flags, layout fine, colours
  held. All installed. 7 tiles whose original repeats cleanly had worse joins; fixed with the
  new tools/seam_fix.py (the edge-average blend, as a tool; band = tile/16): e.g. 710a TB
  4.97 -> 1.68 (orig 1.14), 9d70 4.11 -> 1.46 (orig 0.47), now near the originals.
  buildings-3 in progress (8/54); it's the last queued job. Next new job when it nears the end:
  car atlases (004b6959).
- Check-in 06:23: buildings-3 at 40/54. Queued cars-1 (#15): 2 car atlases at 128 (42ec brown
  sedan, 51a6 wood-panelled wagon/van), wheels a279 (4 faces) and 86f4 (64x64), and 8 dark,
  grimy 256x256 vehicle atlases (06b8 3c9e 532b 9582 9f9b a962 ab58 bdf3; a962/ab58 show a
  dark car top view with red tail lights). Not yet: 49d3 (train), 4ccc (jet), c8d6 (people
  in suits), e996 (misc parts), 7af9 (cow). Car atlases look upright in the dump (the pilot
  wagon's sides have their wheels at the bottom), so no flip. Outputs at 4x (1024 for the
  256s; hdtex accepts up to 4096).
- Check-in 07:00: buildings-3 done (54). Colours and brightness held; 3 low layout scores are plain
  panels. All 54 installed, plus seam_fix on 14 whose originals repeat cleanly. All ~154
  facades are now done. cars-1 in progress (4/12). Next new job when cars-1 nears the end:
  signs (STOP, P, H, F, hot dog, lightning, checkers, newspaper, OCEAN GAS...), and the
  leftover snow-rock tiles.
- Check-in 07:23: cars-1 done (12, with NOTES). Regions kept (layout 0.80-0.99), brightness
  -9..+9, the dark sheets stay grimy, wheels centred. ChatGPT's liberties: wood-like grain on
  some damaged panels (3c9e, 532b, bdf3), heavier rust, and outlines possibly a few px off.
  UV fit is unverified; installed all 12, and the user should check the cars in game (the
  pilot wagon 81fd too). Queued signs-1 (#16, 25: OCEAN GAS, STOP, H, P, F, lightning, hot dog,
  newspaper, traffic light, cone, chequers, coloured windows, manhole, riveted plate, leaded
  glass...). The signs read correctly in the dump, which confirms world tiles are stored
  upright. Also queued ground-snow-2 (#17, the 11 snow-rock tiles the first job missed),
  asking for craggier rock than ground-snow's flat brown.
- Check-ins 08:00 and 08:23: no output for signs-1 or ground-snow-2 since they were queued at 07:23. ChatGPT seems to have stopped watching QUEUE.md (its last delivery was cars-1 by ~07:20). The user needs to re-prompt it.
- ~09:10: the user re-prompted ChatGPT. It can't poll (no scheduler), so it only works while
  answering a message. So: keep the queue deep enough that one prompt runs for hours. The
  README now says to stop and say so when the queue is empty. Queued vehicles-2 (#18: train
  49d3, jet 4ccc, parts sheet e996, cow 7af9). Overnight tally installed: ground (grass 36,
  dirt 42, snow 9, plus fixes), trees 6, walker 16 (feet aligned), fire 36, facades 154 (with
  seam fixes), cars 12, diagonal border fix 2.
- Check-in 10:00: signs-1 (25) is excellent: text, symbols and colours kept; the 2 low layout
  scores (cyan window 4f24, bottle crate fd8b) look right. ground-snow-2 (11): craggy rock this
  time. vehicles-2: train 49d3, jet 4ccc and cow 7af9 good. The parts sheet e996 came back with
  a black background (orig light grey, -62) and moved parts (layout 0.11), so it was sent back
  as parts-fix (#19). Installed 25 + 11 + 3. Queue: just parts-fix. Most of the dumped world
  is now covered; left are water/shore tiles (deliberately excluded), flat fill colours,
  and the 16x16 particles.
- Queued surfaces-1 (#20): the 33 remaining non-water 32x32 tiles (roof gravel/tar, stucco, plain brick, painted panels, gravel, hedges, crate, pond, steel panel, 2 mechanical); skipped the pure white tile. After this, only water/shore remains in 004b6da1.
- Check-in 10:23: parts-fix done; grey background restored, parts in place (layout 0.74, brightness -18; the vents lost a slight teal tint). Installed. surfaces-1 at 32/33.
- Perf (2026-10-28 ~10:30). Logs: even at 61 the in-game medians are 69-78 fps with 13-15 ms of
  work per frame and 40-50 ms worst frames in every session, so it's not only the draw-distance
  tests. My HD layer did linear scans: hd_lookup_redirect on every glBindTexture (thousands per
  frame) over up to 1024 redirects, and hd_object over up to 4096 objects per upload. These
  grew with the overnight installs (449 replacements). Now O(1): hd_redirect is a direct table
  indexed by game name (< 65536), and hd_object_slots is a key hash (open addressing) to the
  object index; hd_forget_all on a fresh context. Plus a background preload (hdtex.c): a
  below-normal thread decodes every replacement at startup; hdtex_load takes the decoded
  pixels if ready (lock-free per-slot state PENDING/DECODING/READY/TAKEN), else decodes now as
  before, and a worker result that loses the race is freed. It logs "hd textures: preloaded N
  in X ms (M MB decoded, waiting for upload)". Decoded-but-unused pixels stay in RAM (~150 MB
  max, 32-bit process: watch it). test_hdtex extended (preload, then decode again): PASS.
  Game ini back to worldtiles=61 for a clean comparison with the 61-tile sessions.
- surfaces-1 done (33): plain roofs, walls and panels; brightness within 15 (low layout scores
  = uniform tiles). Installed, plus seam_fix on 8 (e.g. aa69 TB 7.07 -> 2.17, orig 1.34).
  The queue is empty. Every non-water 004b6da1 world tile plus the 004b6959/004aed7d
  sprites/atlases seen so far are now ChatGPT repaints.
- ~11:00: the queue is empty and every overnight priority (facades, cars, signs) is done; the user is awake, so the overnight cron (4cfceef7) was cancelled. ChatGPT jobs are now reviewed on request.
- Perf test result (session 2026-10-28 11:08, worldtiles=61, O(1) HD lookups + preload): the
  preload decoded 449 in 1266 ms (144 MB) at startup. In game: fps median 72.8, work 13.5 ms
  (render 12.2), p90 21.3, unchanged from before, so my lookups weren't the bottleneck; the
  game's own renderer is (12-21 ms downtown). The big stalls are at startup/menus/loading;
  the one in-game 1.6 s stall (stack 0x46e539 / 0x445ebb / 0x4413c3) is the Apocalypse map's
  scripted opening explosion that turns many buildings to rubble (user), a one-off map
  rebuild that also appears in earlier sessions. Next perf step if wanted: the renderer itself
  (font/sprite re-upload caching, batching).
- User reports road misalignments (screenshots 11:10, 11:16). Candidates seen: the diagonal
  tile's curb is wider than the straight road's curb, an asphalt tone/grain seam between the
  diagonal and straight tiles, and sidewalk paver joints not continuing. Asked which.
  Possible fix in code: build the diagonal tile from the straight-road HD textures.
- User: the misalignment is the curbs. Measured: the straight roads' curb is 16/128 px (cols
  0-15 and 112-127); the original diagonal 8e0c crosses its top and right edges with the curb
  at the same pixels (0-15 / 110-127), so straight roads join there, and its curb is about 16
  px perpendicular (11 samples along the anti-diagonal x sqrt2). The current diagonal matches
  at the borders (thanks to the band blend) but narrows to about 11 px mid-tile, so it kinks.
  Queued roads-curb-fix (#21) with the user's context (one diagonal tile, mirrored/rotated
  copies form the road), ASSEMBLY_ORIG/CURRENT previews (straight above the diagonal, the
  diagonal, its rot180 to the right, the diagonal below, the horizontal straight), in-game
  crops, exact numbers, and a request to self-check with the same assembly. If ChatGPT
  can't hold the curb exact, fallback: synthesize the curb band in code from the straight
  road's curb texture rotated 45 degrees.

- roads-curb-fix: ChatGPT has repeated trouble putting the curb exactly on x=y even with steering (generators are weak at exact geometry). The user is letting it keep trying for now. Fallback if it still fails: keep its road surface, then draw the curb band in code from the straight road's curb (16/128 px wide, perpendicular to the road), placed exactly on the diagonal.
- roads-curb-fix installed. My request was wrong: the original curb is 16 px measured horizontally (x-y 0..16, about 11 px perpendicular), not 16 px perpendicular. ChatGPT caught it and kept the original geometry. It built the curb with an exact script (seam on x=y, curb profile taken from the straight road) and generated only the asphalt. My assembly check shows continuous joins.
- The user had ChatGPT narrow the small curb corner of the diagonal by 4 HD px (x-y 446..511) to match the width seen in game. Intentional; keep it.
- Night tiles: the Apocalypse session (2026-10-28) dumped 319 new 004b6da1 32x32 tiles: the night set (dark ground, lit windows, F/H/P signs). They are separate textures, not a colour lookup of the day tiles. A per-pair colour-mapping consistency test found only 6 matches at 0.97 or better; brightness-structure correlation found 88 pairs above 0.9, mostly ground. With no HD replacement they render as the 1997 originals. Keys: scratchpad new_tiles.txt; pairings: night_pairs.txt.
- Unconverted status: radar 004ae848 and font 004abb2d are skipped on purpose. 49 tiles are ESRGAN only (28 water/shore, the white tile, particles, odds). There are 319 night tiles.
- Night set handled (2026-10-28):
  * All 319 night tiles were ESRGAN-upscaled into hd/ (upscale_textures.py --only 004b6da1), so night is HD right away. The 26 night water/shore tiles stay ESRGAN, the same as the day water.
  * ChatGPT jobs #22-27 queued: night-ground-1 (44 grass/dirt), night-ground-2 (57 rock/snow/grey), night-buildings-1..4 (48 facades each).
  * Per-tile references: <key>__DAY_VERSION_HD.png where the night tile matches a finished day tile. Ground matches use luminance correlation of 0.85 or more; facades use brightness-normalised edge correlation of 0.6 or more, and pairs at that level checked correct visually. Coverage: 9/44, 16/57, 31/22/22/25 of 48.
  * Requests say the night original decides layout, colour and brightness; lit windows stay lit in their warm colour.
  * Installing backs up the ESRGAN night versions to hd_backup_esrgan.
  * Night tiles keyed by sheet index: scratchpad new_tiles.txt (mtime order).
- Check-in cron b03076e6 (every 30 min at :13/:43, session-only): reviews and installs finished night jobs and keeps the queue from running dry.
- night-ground-1 installed (44). Layout 0.68-1.00, brightness within 0 levels: ChatGPT matched average RGB per tile. All 12 seam flags are blend tiles whose originals score higher. ChatGPT left __draft PNGs in out/; the installer ignores them.
- night-ground-2 installed (57). Brightness exact. The 2 low-layout flags are plain snow (flat dither; the score means nothing there). night-buildings-1 in progress (35/48).
- night-buildings-1 installed (48). Brightness exact. The sheet shows lit and dark windows preserved. The lit-mismatch heuristic in scratchpad review_job.py is over-sensitive (threshold edges), so treat it as a pointer only. night-buildings-2 at 38/48.
- night-buildings-2 installed (48). Brightness exact, windows preserved. Fix-up candidates: purple dither specks left on blue walls 4aa479c4, a74651c5, e98738aa (full keys in jobs/night-buildings-2/out). night-buildings-3 at 38/48.
- night-buildings-3 installed (48). The c57b07eb low layout score is a flat blue panel with sparse dither, so it is fine. Queued #28 night-fix-1: remove the purple specks from 4aa479c4, a74651c5, e98738aa and ca086d94, starting from the installed results (__PREVIOUS_ATTEMPT). night-buildings-4 at 29/48.
- night-buildings-4 installed (48; F/H/P signs good). night-fix-1: ChatGPT removed the purple specks (0.3-1% of pixels changed, edges identical). The specks on tile borders were then cleared in code with a wrap-aware neighbour fill, and the fix installed. All 6 night jobs are done: 293 ChatGPT night tiles plus 26 ESRGAN water/shore.
- Check-in cron b03076e6 cancelled: the queue is empty.
- Defender quarantined game/streets_launch.exe on 2026-10-28 at 11:16 (Behavior:Win32/DefenseEvasion.A!ml: an ML behaviour flag on the DLL injection). A copy is in fix/. Fix: the user adds a Defender exclusion for game/, then copy the exe back. Longer term: replace injection with a proxy DLL the game loads itself. This matters for the brothers' machines.
- version.dll proxy attempt (fix/version_proxy.c): it exports the 3 VERSION functions Streets.exe uses, and its DllMain patches the exe entry point with a jmp to a stub that loads streets_fix.dll. The first test launch hit the CD prompt, meaning streets_fix did not load. Root cause unknown; the trace-logging build has not been tested. The proxy is removed from game/ (fix/version.dll.disabled) and the Play .cmd files point back at streets_launch.exe. The user allowed the launcher in Defender, and a launcher start at 22:13 loaded the fix correctly.
- In-game check (2026-10-28, launcher): the diagonal curbs look fine to the user. Night tiles are not seen in game yet.
- dsound.dll proxy works (fix/dsound_proxy.c, 2026-10-28):
  * version.dll failed because Windows applies the DWM8And16BitMitigation compat shim to Streets.exe, and the shim loads System32 version.dll first.
  * dsound.dll passes through all 12 exports of the system dsound.dll by name and ordinal (naked jmp thunks, drectve -export:Name=p_Name,@ord).
  * Its DllMain patches the exe entry point with a jmp to a stub that restores the bytes, sets the ddraw.ini renderer from -o2, and calls LoadLibrary(streets_fix.dll). It traces to logs/dsound_proxy.log.
  * Verified: direct Streets.exe -o2 start, all patches ok, no CD prompt.
  * The Play .cmd files now start Streets.exe directly. streets_launch.exe still works alongside (LoadLibrary refcount). The ini renderer= option only applies via the launcher.
  * Build: zig cc -target x86-windows-gnu -O2 -shared -o dsound.dll dsound_proxy.c -luser32
- User saw "dirt on a hill, possibly rubble" still looking low quality. Found 2 ESRGAN-only textures left out of every batch: 93158f420cfa69cc (dirt/grass ground; ESRGAN turned the speckle into vertical streaks) and 330eab6232d30b0c (rubble chunk sprite; ESRGAN made a smooth blob with a dark ring). Queued #29 ground-rubble. Checked the 54 ESRGAN world tiles: everything else is truly water or shoreline (blue fraction 0.17 or more), plus the white tile.
- Water/shore queued (user asked):
  * #30 water-1: 4 textures, one per water group. Groups by colour: day_a (29,59,76), day_b (34,63,81), night_a (25,26,42), night_b (29,28,50); 5 frame keys each in scratchpad water_groups.json. The night session load order shows night_b = names 2-6.
  * Install plan: copy each result to all 5 keys of its group. With hdwater=1 only frame WATER_HD_FRAME is shown, scrolled by waterdrift, so the textures must tile seamlessly.
  * #31 shore-1: 32 static shoreline blend tiles (16 day, 16 night); 4f7567 is a teal pond-like tile.
- Installed #29 ground-rubble (2), #30 water-1 (4 textures, each copied to all 5 frame keys of its group; ESRGAN frames backed up) and #31 shore-1 (32). ChatGPT built shore tiles with scripted masks from the original palette plus the water-1/ground textures. Low layout scores are dither vs smooth; the shapes are checked visually. 4f7567 (green pond water) had been made blue; match_tone restored the green (gain r1.37 g1.20 b0.89). Now ESRGAN-only: the white tile plus about 20 small sprites/particles.
- Moose replaces the cow (user request, 2026-10-29):
  * The cow is object 505 "Line01" in GEO/SIM3D1.MAX: 66 verts, 101 faces, all type 13 with texture 168 = SIM3D.BMP #168 = key 7af9800f2ba8ce79.
  * SIM3D.BMP layout: u32 size, 0, count, count, then count x (w,h,flags); each texture is (w,h,flags), h x u32 row offsets, then pixels. The FNV keys of its 171 textures match our dumps (131 dumped).
  * MAX conventions: metres x 262144, feet y=0, head -z, outward winding (v1-v0)x(v2-v0). UV 16.16 with file v = 1 - (image row / height) in dump/HD orientation. Face unk field = face index.
  * Tables: GEOM entry (53 B) rendered@29, faces@41, unique@45; duplicate entry (36 B) id@0, addr@4, rendered@12, faces@24, unique@28. All 184 entries verified consistent. tools/maxmesh.py parses objects.
  * tools/make_moose.py builds a 156-face moose (lofts, palmate antlers, dewlap) with side-projection UVs plus an antler band. "install" patches SIM3D1.MAX (backup SIM3D1.MAX.orig), shifts later addresses, and updates counts and totals.
  * Placeholder HD texture installed at hd/7af9800f2ba8ce79.png (ChatGPT cow still in jobs/vehicles-2/out). The texture is queued as #32 moose-1. The 8-bit texture 168 is unchanged, so without HD the moose wears cow patches.
- Moose revision 1 (user critique): legs now start inside the body (thigh/shoulder section, no gap); antlers tilted back 25 degrees (ANTLER_TILT) about the skull. 216 verts, 172 faces. Reinstalled; UV guide refreshed in jobs/moose-1/in.
- Moose revision 2 (user: antlers not right, nose like an aardvark): antlers rebuilt per anatomy (Wikipedia/Alces/Boone and Crockett: short thick beam from the skull top, forward brow palm with forward points, big main palm dished palm-up sweeping out and back with rim points, bay between, spread ~1.5 m; the tilt hack is removed). Muzzle is now big, blunt and overhanging (wider and deeper than the skull). Ears moved behind the antlers. 278 verts, 218 faces. Reinstalled.
- Moose revision 3: antler palms and points are now solids (ANTLER_T 5 cm slabs via slab()/point(), wound outward per solid); both palms start at the beam end, so they are attached. Dewlap is a tapered 6-sided cone hanging from inside the throat. 279 verts, 301 faces. Reinstalled.
- Moose revision 4: the beam was flat front to back (ring() cross-sections faced along z). Added ring(sideways=True) for parts running along x; the beam is now a 6-sided solid ~15 cm thick. 287 verts, 305 faces. Reinstalled.
- Moose revision 5: ears are solid leaves (4 cm thick, 8 faces each). Beam thinned to ~10 cm and now starts at the skull centre below its top, so it grows out of the head. 299 verts, 317 faces. Reinstalled.
- moose-1 request rewritten for the final model (rev 5): side-view band (head left, dewlap, ear, hooves at ~67%), antler band from ~74% (beam strip on the left, brow lobe, main palm), fill rules, colours. Inputs refreshed: UV_LAYOUT, PLACEHOLDER_COLOURS, MODEL_RENDERS. Queue #32 pending.
- moose-1 done: ChatGPT texture fits the UVs (QA overlay); the Blender render looks right. Installed to hd/7af9800f2ba8ce79.png and models/moose_texture.png. Known flaw: rump and chest end faces get streaks from the side projection; fix with a dedicated rear/front UV region if wanted.
- The user approved the textured moose in Blender ("looks like a moose!"). models/moose.blend now opens in Material Preview with the texture packed. Next: in-game check (find a cow/moose on a farm map).
- Moose sounds (2026-10-29): SOUND/cowhit.WAV and cowland.WAV replaced (originals kept as .WAV.orig). Source: public domain US Fish and Wildlife Service female moose calls from Minnesota via SoundBible (#1225 grunts, #1227 moans). cowhit = first grunt of #1225 (0.12-0.85 s, tanh-compressed to the original loudness); cowland = opening of the loud moan in #1227 (1.02-1.95 s). 8-bit mono 22050 Hz like the originals. Acoustic Atlas/WSA moose recordings are CC BY-NC-ND (no derivatives), so not used.
- Moose in game works (screenshot 2026-10-29 10:47): loads, textured, no crash. Bug: the hind legs and parts of the palms vanished from some angles because the game culls back faces and 72 faces were wound inward (42 antler, 12 ear, 16 leg, 2 head). Cause: orient() compared each face against the centre of its whole part, which misjudges long bent lofts and thin wide solids. Fix: loft quads use their own segment centre; thin solids (palm slabs, points, ears) use facing_away(face, opposite face). Verified with a Blender back-face-culled render from 6 views; reinstalled.
- cowhit stutter (user: "looped the first .1s before playing the whole wav"): the game likely retriggers the hit sound for a few frames while the car pushes the cow; the original moo starts with 40 ms of silence and a soft ramp that hides this. Fix: removed the 70 ms breathy lead-in, 20 ms fade-in, and 150 ms of leading silence. Not yet verified in game; if it persists, find the retrigger in code (string "cowhit" in Streets.exe).
- Moose sounds deepened (brother: "too much like a minecraft villager"): tools/make_moose_sounds.py rebuilds both from the SoundBible sources. PITCH 0.70 (~6 semitones down, resampled so also slower), 5-tap low-pass, tanh to original loudness; cowhit keeps 150 ms lead silence. The source WAVs are in the scratchpad moose_dl (re-download from soundbible.com/grab.php?id=1225|1227&type=wav).
- The user approved the deepened moose sounds ("sounds great"). PITCH 0.70 is final.
- The user declined the moose rump texture fix ("we gotta have a little jank in this 30 year old game"). Do not re-suggest it.
- Draw distance resumed (2026-10-29). Ruled out: the 0x7800000 uses (FUN_004e8410/4f4e20/4fb1d0/508300 are sound distance attenuation, channel 0x27) and the 0x3d strides at 0x9bfd70/0x9bea10 (screen-space coverage grids, already resized by widescreen). Game ini worldtiles=63 for the live ground: comparison.
- FOUND the 63+ missing-ground cause: the window origin was not recentred. FUN_004dd240 (draw distance setting; "max" at launch) recentres (origin += (old-new)/2) only if the new size equals the requested one (cmp esi,eax; jne at 0x4dd28f). grid_resize mapped 61 to dd_tiles, so the check failed and the origin stayed at the 25-tile startup value: camera 12 tiles from the corner of a 63 window (log: origin (76,3), camera (88,15)), with most of the window behind the car. Same mapping broke the , key (63-2=61 mapped back to 63). Fix in grid_resize: map 61 only when growing (old<61); when mapped, shift the origin by (old-now)/2 itself. The , / . key sites nudge only on a change of exactly 2, which still works for real steps. Deployed with worldtiles=63 for the test.
- Recentering fix verified (session 11:12, worldtiles=63): origin (57,-16), camera (88,15), so the camera is centred (31,31). User: "looks good now". 63 tiles costs 100-117 fps, render 7-9 ms. Game ini now worldtiles=91 for the next test.
- The , / . keys do nothing in game: FUN_004dcf10 runs every frame (from FUN_004a7960) and reads key state [0x5e82e8]+0x2f6+DIK*2 for 0x33/0x34, ungated, but the state never shows them pressed. Probably only keys bound in input.cfg are tracked. My earlier note claiming they work came from reading code, not testing. Not needed: draw distance comes from ini worldtiles. Option: own hotkeys in the DLL.
- 91-tile session (11:15): window centred (origin (43,-30), camera (88,15)); ~1700 visible cells, ~9k polygons (61: ~900 / 3.5k); 80-100 fps, render 9-12 ms. Screenshot shows the whole skyline; the ground ending at the sides was the MAP EDGE (camera at map row 15 of 128), not the window. The user was unconvinced because there was no A/B. Added Ctrl+Shift+F9 (drawdist_toggle): switches 61 <-> worldtiles between frames in drawdist_frame, using the game resize steps (FUN_004c56d0, far clip FUN_004d16a0((n/2-3)*0x400000), origin += (old-new)/2, FUN_004dd4e0). Only active when OpenGL widescreen runs drawdist_frame. Deployed.
- User on Ctrl+Shift+F9 A/B: "oh yeah thats noticably better" (91 vs 61). The toggle works (log 11:19). FPS at 91 toward the city centre: 40-75, often 40-48 (render 20-25 ms); one 17 fps hitch. Below the 60 the user accepted.
- Ctrl+Shift+F9 now cycles 61 -> middle -> worldtiles -> 61 (middle = (61+worldtiles)/2|1, so 77 at 91). The user wants all three selectable. Launch still starts at worldtiles (91). Deployed, untested.
- Default start is now 77 (user): new ini worldstart (0 = middle (61+worldtiles)/2|1); grid_resize maps the launch "max" (61) to dd_start; it skips the self-recentre when dd_start==61 (the game recentres itself then). The cycle is unchanged (61/middle/max). Game ini: worldtiles=91, worldstart=0.
- Open-items review (2026-10-29, user asked "what else do we need to fix"): untested = 77 start, cowhit stutter, "YOU WON!" box (ddraw8bpp), car UV fit; never checked = glow sprites (FUN_004ade20, Glide-only branch, OpenGL may draw them invisible), Apocalypse white flash in OpenGL, explicit width/height ini; known = 91-tile fps 40s toward city, far flicker (original painter sort, accepted); housekeeping = repo, packaging dsound proxy + ini for the brothers.
- Particles were missing in OpenGL (2026-10-29). FUN_004ade20 ("glow sprites" in earlier notes) is really the smoke/fire/spray/debris particle effect: called for render-list kind 0x1a with &0x9bb358 (e[1] = projected vertex: x>>12, y>>12 pixels, z; e[3] = type 0-11). Glide-only (returns unless renderer 1). Per type: size f = (0x9780000 - z)/0x9780000 (16.16, FUN_004ab0fb); min/max half-size, count (cap 30) and spread from the jump table at 0x4ae2d4; each square at centre + rand%(2*spread) - spread, size (rand%(max-min+1)+min)*2, skipped past 639/479. Textures are exe resources 0x7c spray (types 0,3; name at 0xb272c0), 0x7d smoke (1,4,6,7,11; 0xb27260), 0x7e debris (8,9; 0xb27a30), 0x7f fire (2,5,10; 0xb27a10; Glide turns fog off), uploaded by FUN_004b69b0 (glTexImage2D ret 0x4b6b34) in OpenGL too; HD versions already exist (0f54af13.., 2e4ab660.., e403355f.., e88f4e86..). Glide: chroma key black, alpha blend, texture colour only. Now drawn by opengl.c particles() (detour like fill_rect): soft key (dark texels fade out, uploads and HD), fire additive, sprite-style depth test, positions re-rolled at 30 Hz (own RNG, game rand untouched). Untested.
- Apocalypse flash black in OpenGL: FUN_0046e310 steps the palette +4/channel 64 times (WaitForVerticalBlank per step), each step blitting with an alternating 8-line shake via this+0x5c (cnc-ddraw's empty 8-bit screen covered the window = black), then redraws twice and fades back from white (thiscall FUN_00525fa0(0xffffff, 4000 ms, 32 steps) at 0x46e66f, target palette *(0x6efaf4)+0x2a). Now in OpenGL: trampoline at 0x46e479 calls flash_whiteout (copies the front buffer, draws it with additive white 4k/255 and the shake, 60 Hz, then holds white); opengl_before_swap (SwapBuffers hook now always on) copies each redraw and clears white; flash_fade draws the 4 s fade from the copy, then runs the original (palette only, capped). Copies disable GL_MAP_COLOR (the game's colour-index pixel maps). Safety: white released after 5 swaps without a fade. Untested.
