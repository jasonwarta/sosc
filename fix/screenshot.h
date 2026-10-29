// Screenshot of the game window, saved as <game_dir>\screenshots\shot_<time>.png.
#pragma once
#include <windows.h>

void take_screenshot(HWND window, const char *game_dir);

// Save an RGBA image (rows bottom-up, pixels 0xAABBGGRR) as PNG; returns nonzero on success.
int save_rgba_png(const unsigned long *rgba, int w, int h, const char *path);
