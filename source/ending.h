// re2ds - the ending (COMMON/BIN/ENDING.BIN): the last movie, the picture
// and the results (rank, clear time, saves), then back to the title.
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t frames;            // play time in 1/60 s
    unsigned saves;             // typewriter saves of this game
    unsigned sprays;            // first aid sprays used
    int claire, scenario_b;     // flags 1:0 and 1:1
    int special_weapon;         // flag 1:8: a special weapon was used (50 points less)
} EndingStats;

// Runs the whole ending on the upper screen; work is a 256 x 192 buffer.
void ending_run(const EndingStats *st, uint8_t *work);

// The rank the PS1 gives (0 = A .. 4 = E) and the parts of its score (for the docs / tests)
int ending_rank(const EndingStats *st, int *time_points, int *save_points, int *spray_points);
