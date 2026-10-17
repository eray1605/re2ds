// re2ds - enemies and other characters set by the room scripts (sce_em_set).
//
// Models come from tools/em2ds.py (model/emXX.bin, _tex.bin, _sets.bin).
// Every type behaves like a zombie (docs/enemy-zombie.md): lurking,
// chasing, lying and getting up as the spawn variant says. No attacks yet.
#pragma once

#include <stdint.h>

#include "assets.h"

#define MAX_ENEMIES 8

void enemy_room_reset(void);   // frees all enemy models (room change)
// the room's path zones (tools/blk2ds.py, kept by the caller), NULL if none
void enemy_set_zones(const void *data);
// the room's own animations for the enemy in slot (RBJ, tools/rbj2ds.py), NULL if none
void enemy_set_room_set(int slot, const Model *set);
// plc_* script commands for the enemy in slot (plc.h)
void enemy_plc(int slot, const uint8_t *ins);
// sce_em_set: slot, type (0x10.. ), flags (bytes 4..5: variant | status << 8),
// position, facing (RE2 units)
// voice: kind of voice (byte 7), see enemy_sound_set
void enemy_spawn(int slot, int type, int flags, int voice, int32_t x, int32_t y, int32_t z, int16_t dir_re2);
// after the room's spawns: index of its enemy sound set (sound/enemNN.bin)
// from the voice kind pairs of sound/enem_pairs.bin, -1 if none
int enemy_sound_set(const uint8_t *pairs);

#define NOISE_WALK 0x02
#define NOISE_RUN 0x04
typedef struct {
    int32_t x, y, z;
    int16_t dir;                // DS units
    int noise;                  // NOISE_*
    int pressed;                // a key went down since the last tick (struggling free)
    int can_grab;               // walks or stands under control (not climbing, not held)
    int dead;
} EnemyTarget;
void enemy_tick(const EnemyTarget *player); // once per game tick
int enemy_grabbing(void);       // a zombie holds Leon
#define NOISE_SHOT 0x01
// auto-aim: nearest living enemy Leon sees within range (0 = any); -1 none
int enemy_aim_target(int32_t x, int32_t y, int32_t z, int range, int32_t *tx, int32_t *tz);
// shot / stab toward dir: damage[near, mid, far]; returns the enemy hit or -1
// low: Leon aims down. Zombies on the ground (knocked down or lying) are only
// hit then - a straight shot passes over them, as the PS1's hit check
// compares the heights of the body parts (FUN_8003dd8c).
int enemy_hit(int32_t x, int32_t y, int32_t z, int16_t dir, int range, int weapon, const uint16_t damage[3], int low);
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

void model_gx_cost(const Model *m, int *vertices, int *polygons);
void enemy_debug_print(int tick);   // host tests only
