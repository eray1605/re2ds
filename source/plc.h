// re2ds - characters the room scripts move (plc_* commands).
//
// On the PS1 these commands put an object (Leon, Kendo, ...) into routine 4
// (FUN_80061488) with a mode from table 0x800a0de0: play an animation of the
// room's own set or of the character's, walk to a point, turn to a point or
// a facing, stand. plc_ret hands control back. Modes used by disc 1 are
// implemented; see re2ds-notes/cutscenes.md.
#pragma once

#include <stdint.h>

#include "assets.h"

enum {
    PLC_ANIM_ROOM = 0,    // animation from the room's set (RBJ, obj+0x190)
    PLC_ANIM_OWN = 1,     // animation from the character's own set
    PLC_WALK = 4,         // turn to the destination, then walk there
    PLC_RUN = 5,
    PLC_STAND = 6,        // what the others end in
    PLC_BACK = 7,         // walk backward to the destination
    PLC_FACE = 9,         // turn to face the destination (walking on the spot)
    PLC_PAUSED = 0x14,    // plc_stop
    PLC_TURN_TO = 0x15,   // turn to the facing in dest_x
};

typedef struct {
    uint8_t active;          // routine 4: the room script moves the character
    uint8_t mode, step;      // obj+5 / obj+6
    uint8_t saved_mode, saved_step; // plc_stop
    uint16_t flags;          // obj+0x1cc: 4 loop, 8 double speed, 0x10 half speed,
                             // 0x40 no blend, 0x80 backward
    int16_t rot, rot_end;    // obj+0x1d4 / 0x1d6: turn per tick (RE2 units), at the end
    int16_t dest_x, dest_z;  // obj+0x1c4 / 0x1c6
    uint8_t done_bit;        // obj+0x1cb: bit of flag array 5 set on arrival
    int16_t speed;           // obj+0x144: forward units per tick
    uint8_t anim;            // modes 0 / 1
    uint8_t key;             // obj+0x14d: next keyframe
    uint8_t odd;             // flag 0x10 toggle
} Plc;

// The character as plc_tick sees it; the owner copies its fields in and out.
typedef struct {
    int32_t x, z;
    int16_t dir;             // DS units (32768 = 360 degrees)
    const Model *set;        // shown: set, anim, key
    int anim, key;
    const Model *room_set;   // mode 0: the room's animations for it (may be NULL)
    const Model *own_set;    // modes 1, walking, standing
    int own_first;           // mode 1 animation n = own_first + n
    int walk_anim, idle_anim; // absolute in own_set
    int table;               // speed / turn tables: 16 + player type, em type - 0x40
    int no_blend;            // out: the animation started without easing in
    int footstep;            // out: a footstep keyframe was passed
} PlcBody;

// op = the script instruction (0x3F plc_motion, 0x40 plc_dest, 0x42 plc_ret,
// 0x58 plc_rot, 0x5B plc_cnt, 0x66 plc_stop; plc_neck 0x41 is ignored)
void plc_command(Plc *p, PlcBody *b, const uint8_t *ins);
void plc_tick(Plc *p, PlcBody *b);   // one game tick of routine 4
