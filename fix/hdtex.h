// High-resolution texture replacements (game\textures\hd\<hash>.png).
#pragma once

// Index the replacement directory; returns the number of replacements found.
int hdtex_init(const char *dir);
// Is there a replacement for this texture key?
int hdtex_has(unsigned long long key);
// Start decoding every replacement on a background thread (call after hdtex_init).
void hdtex_preload_start(void);
// Decode a replacement as RGBA (0xAABBGGRR), rows in the game's order. Caller frees. Uses the
// preloaded pixels when the background thread has them, else decodes now.
unsigned long *hdtex_load(unsigned long long key, int *w, int *h);
// Same for any PNG file (NULL when missing or unreadable).
unsigned long *hdtex_load_file(const char *path, int *w, int *h);
