// re2ds - enemies and other characters set by the room scripts (sce_em_set).
//
// Models come from tools/em2ds.py (model/emXX.bin, _tex.bin, _sets.bin).
// Every type behaves like a zombie (re2ds-notes/enemy-zombie.md): lurking,
// chasing, lying and getting up as the spawn variant says. No attacks yet.
#pragma once

#include <stdint.h>

#define MAX_ENEMIES 8

void enemy_room_reset(void);   // frees all enemy models (room change)
// sce_em_set: slot, type (0x10.. ), flags (bytes 4..5: variant | status << 8),
// position, facing (RE2 units)
void enemy_spawn(int slot, int type, int flags, int32_t x, int32_t y, int32_t z, int16_t dir_re2);
#define NOISE_WALK 0x02
#define NOISE_RUN 0x04
void enemy_tick(int32_t player_x, int32_t player_y, int32_t player_z, int noise); // once per game tick
// room script access (work_set 3): member 7 = flags; 0 if unknown
int enemy_member_get(int slot, int member, int *value);
void enemy_member_set(int slot, int member, int value);
// reserve_*: GX RAM still needed after the enemies (the masks)
void enemy_draw(int reserve_vertices, int reserve_polygons);
int enemy_count(void);
// player collision with enemies (walls are checked in main.c)
int enemy_blocks_player(int32_t from_x, int32_t from_z, int32_t x, int32_t z);
extern int enemy_spawn_calls, enemy_last_error;
extern int enemy_jumps, enemy_jump_max, enemy_toggles;

#include "assets.h"
void model_gx_cost(const Model *m, int *vertices, int *polygons);
void enemy_debug_print(int tick);   // host tests only
