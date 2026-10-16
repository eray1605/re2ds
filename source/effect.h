// re2ds - effect sprites (blood, muzzle flash, casings) from effect/core.bin
// (tools/esp2ds.py).
//
// The PS1 spawns them with FUN_8001b8fc (bank, effect, strength) at a body
// part; each sprite plays a sub-animation of its effect while it flies and
// falls. Here: billboards with gravity that stop on the floor.
#pragma once

#include <stdint.h>

#define BLOOD_SPLASH 0          // big splash (bites)
#define BLOOD_SPURT 1           // smaller one
#define BLOOD_DROPS 2

void effect_load(void);
void effect_clear(void);        // room change
// a sprite playing sub-animation `sub` of CORE00.ESP effect `effect` (0 blood,
// 1 gun fire, 2 casing), moving by (vx, vy, vz) per tick, falling with
// gravity, stopping at floor_y; coloured r, g, b
void effect_spawn(int effect, int sub, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int32_t vz,
                  int gravity, int32_t floor_y, uint8_t r, uint8_t g, uint8_t b);
// sprays a blood sprite from (x, y, z) toward dir (DS units); floor_y = where it stops
void effect_blood(int kind, int32_t x, int32_t y, int32_t z, int16_t dir, int speed, int32_t floor_y);
void effect_tick(void);         // once per game tick
int effect_count(void);         // sprites alive (one quad each)
void effect_draw(const int32_t cam_from[3], const int32_t cam_to[3]);
extern int effect_status, effect_spawned; // diagnostics
