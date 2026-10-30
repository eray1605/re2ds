// re2ds - the room's effect sprites (fires, smoke, sparks) that the scripts
// start with sce_espr_on and stop with sce_espr_kill; room/<rdt>_esp.bin
// from tools/resp2ds.py.
//
// On the PS1 sce_espr_on (scd_op_3a) calls FUN_8001b8fc with effect id,
// sub effect | palette row << 3 and a parameter, at a position relative to
// the world or an object. Here: a looping billboard per call, playing the
// sub effect's frames from its first one (time 255 = go to frame n).
#pragma once

#include <stdint.h>

void roomfx_load(int stage, int room);   // frees the previous room's
// ins = the 16-byte sce_espr_on instruction
void roomfx_on(const uint8_t *ins);
// effect id (the room's, else CORE00.ESP's), sub effect | palette row << 3,
// size = the sce_espr_on parameter's high byte (0 = 1:1)
void roomfx_spawn(int id, int type, int32_t x, int32_t y, int32_t z, int size, int dir_re2);
// a sprite thrown from (x, y, z) with a speed per tick, falling until floor_y,
// where it lies a moment; plays the effect's frames from first (casings).
// Its first floor contact calls game_casing_sound(sound).
void roomfx_throw(int id, int first, int row, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int32_t vz,
                  int32_t floor_y, int scale, int sound);
// main.c: a thrown sprite (casing) hits the floor: weapon bank sound
void game_casing_sound(int sound);
// room collision (main.c): a casing turns round at a wall
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y);
void roomfx_kill(int id, int type);      // sce_espr_kill
void roomfx_tick(void);                  // once per game tick
int roomfx_count(void);                  // sprites alive (one quad each)
void roomfx_draw(const int32_t cam_from[3], const int32_t cam_to[3]);
