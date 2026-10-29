// Content mods (mods\ in the game folder) installed into the game's files at startup.
#pragma once

// Install (moose=1) or remove (0) the moose: model patch and sounds, originals kept as *.orig.
void mods_install(const char *game_dir, int moose);
