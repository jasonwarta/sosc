// OpenGL presentation layer for the game's -o2 renderer.
#pragma once

typedef void *(*patch_iat_fn)(const char *dll, const char *func, void *hook);

// Hooks the game's OpenGL imports; returns nonzero when everything was installed.
int opengl_install(patch_iat_fn patch_iat);

// Enable image dumps (overlays and textures) into dir.
void opengl_set_dump_dir(const char *dir);
// Save every distinct uploaded texture once (keyed by a hash of its 8-bit indices) and log
// per-call-site upload counts: the census for high-resolution texture replacement.
void opengl_set_texture_census_dir(const char *dir);
// Load high-resolution replacements from dir (<hash>.png). Call after opengl_install.
void opengl_set_hd_dir(const char *dir);
// Stretch the edges of full-width cockpit images to the screen sides in widescreen (default on).
void opengl_set_extend_hud(int on);
// Hand-made wide versions of full-width overlays: dir\<w>x<h>.png or dir\<hash>.png.
void opengl_set_wide_dir(const char *dir);
// Diagnostics: log which texture names are bound in only some frames (animations).
void opengl_set_tex_probe(int on);
// Call once per game frame (main loop tick).
void opengl_frame_tick(void);
// Water animation step in ms (0 = leave it on game frames).
void opengl_set_water_ms(int ms);
// Water drift with hdwater=1, in texels of the original 32-texel tile per second.
void opengl_set_water_drift(int texels_per_s);
// Use HD water (one HD frame drifting smoothly instead of the five flipbook frames).
void opengl_set_hd_water(int on);
// Depth-test billboard sprites against the world (on by default).
void opengl_set_sprite_depth(int on);
// Call when the game creates a GL context: cached GL state no longer applies.
void opengl_context_created(int fresh);

// Call just before the game's SwapBuffers (the Apocalypse flash holds white over redraws).
void opengl_before_swap(void);

// Call every frame: hooks the game's DirectDraw palette once it exists, to follow its colours.
void opengl_track_palette(void);
