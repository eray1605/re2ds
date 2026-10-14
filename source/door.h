// re2ds - door sequence: plays the program of a door file (DOORnn.DO2) the
// way the PS1 door task does (0x800136dc: setup 0x80013734, loop 0x800139cc,
// end 0x80013c94).
#pragma once

#include <stdint.h>

// Loads door file n (door/doorNN_*.bin, sound/doorNN.bin). Returns 0 if missing.
int door_begin(int file, int variant_byte);
// One display frame: runs the program at the PS1 rate, renders the door.
// Returns 0 once the sequence has ended.
int door_frame(void);
// Frees everything; plays the closing sound if the door asked for it.
void door_end(void);
