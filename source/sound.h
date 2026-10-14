// re2ds - sound banks converted from PS1 VABs (see formats.h, SoundBankHeader).
#pragma once

#include "formats.h"

void sound_init(void);
// Loads a bank; returns NULL if the file is missing.
void *sound_bank_load(const char *path);
void sound_bank_free(void *bank);
// Plays the sample whose key range contains note; returns the DS channel or -1.
int sound_play(const void *bank, int note, int volume, int pan);
// Length in DS frames (60 Hz) of the sample for note, 0 if none.
int sound_frames(const void *bank, int note);
