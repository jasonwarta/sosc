// Draw distance beyond the game's 61-tile maximum.
#pragma once

// The world window at the game's "max" draw distance, and the per-frame list capacities.
extern int dd_tiles, dd_poly_cap, dd_vertex_cap;

// Let the draw distance window grow to `tiles` map tiles across (odd, up to 125), starting at
// `start` tiles (0: halfway between 61 and tiles). Call before the game starts. Returns -1
// when tiles <= 61 (nothing to do), 0 on failure (nothing patched), 1 when installed.
int drawdist_install(int tiles, int start);

// Peak bytes used so far in the two terrain buffers (0 when not extended). Called once per
// frame on the game thread; also carries out a pending drawdist_toggle.
void drawdist_frame(int *used_a, int *used_b);

// Ask for the window to step to the next size (61, the middle size, the extended size) at the end
// of the next frame (any thread).
void drawdist_toggle(void);
