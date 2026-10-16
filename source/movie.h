// re2ds - the PS1 movies (FMV), converted by tools/str2ds.py.
#pragma once

// Plays nitro:/movie/<name>.bin on the upper screen, letterboxed, with its
// sound. START skips. Returns 1 if skipped (or the file is missing), 0 if it
// played to the end. Leaves the upper screen black with the 3D layer off
// until movie_end() restores the game's display.
int movie_play(const char *name);
void movie_end(void);
