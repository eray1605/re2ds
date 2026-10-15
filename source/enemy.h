// re2ds - enemies and other characters set by the room scripts (sce_em_set).
//
// Models come from tools/em2ds.py (model/emXX.bin, _tex.bin, _sets.bin).
// For now every type walks like a zombie (re2ds-notes/enemy-zombie.md,
// state 1): one of the 8 gait animations of set 0, root motion forward,
// turning toward the player. No attacks yet.
#pragma once

#include <stdint.h>

#define MAX_ENEMIES 8

void enemy_room_reset(void);   // frees all enemy models (room change)
// sce_em_set: slot, type (0x10.. ), spawn variant, position, facing (RE2 units)
void enemy_spawn(int slot, int type, int variant, int32_t x, int32_t y, int32_t z, int16_t dir_re2);
void enemy_tick(int32_t player_x, int32_t player_z);   // once per game tick
// vertices/polygons_used: what the frame already sends (Leon, masks)
void enemy_draw(int vertices_used, int polygons_used);
int enemy_count(void);
extern int enemy_spawn_calls, enemy_last_error;

#include "assets.h"
void model_gx_cost(const Model *m, int *vertices, int *polygons);
void enemy_debug_print(int tick);   // host tests only
