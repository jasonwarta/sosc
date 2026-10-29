// Widescreen support for the game's OpenGL renderer (-o2).
#pragma once

// Width of the game's 2D coordinate space: 640 originally, 480 x aspect when widescreen.
extern int ws_canvas_w;
// 2D overlays (dashboard, radar, text, mirror) are laid out for 640 and are drawn shifted
// right by this much, so they stay centred.
extern int ws_hud_off;

// Choose the coordinate space for a screen of the given aspect ratio (width / height) and
// patch the game's view setup. Call before opengl_install. Returns nonzero when widescreen
// is active.
int widescreen_install(double aspect);

// Bounds check for the screen-coverage grids (all renderers, with or without widescreen):
// polygons with off-screen vertices are skipped instead of indexing past the grids.
int grid_guard_install(void);

// Call every frame (logs peak per-frame polygon and vertex counts).
void widescreen_frame(void);
