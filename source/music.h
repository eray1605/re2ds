// re2ds - background music: plays the PS1 SEQ sequences of music/*.bin
// (tools/bgm2ds.py) on the DS sound channels, one note per channel.
#pragma once

#include <stdint.h>

void music_play(const char *path);  // starts it (the old one stops); missing file: silence
void music_stop(void);
void music_frame(void);             // every DS frame (60 Hz)
int music_playing(void);
// Room music like the PS1 (FUN_80055ed8): the BGM table entry of the room
// picks MAINxx; the same music keeps playing across rooms.
void music_room(int stage, int room);
void music_control(int id, int op, int type, int left, int right); // sce_bgm_control
void music_duck(int on);      // quieter while a voice clip plays
