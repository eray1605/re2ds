// re2ds - subtitles for the spoken lines and the movies (subs/subs.txt from
// tools/subs2ds.py: speech recognition over the player's own disc audio; the
// English disc itself has no text for them).
#pragma once

#include <stdint.h>

void subs_init(void);           // loads subs/subs.txt if it is there
// the line for clip key ("s2_13", "c_04", a movie name) at ms into it, NULL if none
const char *subs_at(const char *key, uint32_t ms);
int subs_have(const char *key); // any lines for that clip
