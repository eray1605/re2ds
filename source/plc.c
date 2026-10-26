// re2ds - characters the room scripts move (see plc.h).
#include <math.h>
#include <stdlib.h>

#include <nds.h>

#include "plc.h"
#include "script.h"

// Per character type (index 16 + player type, or enemy type - 0x40): forward
// speed of walking (0x800a0c90, even bytes) and backing up (0x800a0cd0), turn
// rate of the walk / back / face / run modes (0x8009f9dd, 0x8009fa1d,
// 0x8009fa5d, 0x8009fadd: odd bytes). Leon is 16, Kendo (0x48) 8.
static const uint8_t walk_speed[32] = { 20, 70, 20, 70, 60, 50, 60, 65, 60, 65, 30, 0, 0, 0, 0, 50,
                                        75, 70, 75, 70, 75, 70, 75, 70, 75, 70, 75, 70, 75, 75, 70, 50 };
static const uint8_t back_speed[32] = { 25, 55, 25, 55, 55, 35, 55, 55, 55, 55, 25, 0, 0, 0, 0, 35,
                                        70, 65, 70, 65, 70, 65, 70, 65, 70, 65, 70, 65, 70, 70, 55, 35 };
static const uint8_t walk_rot[32] = { 16, 48, 16, 48, 48, 48, 48, 48, 48, 48, 16, 0, 0, 0, 0, 48,
                                      48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48, 48 };
static const uint8_t face_rot[32] = { 32, 88, 32, 88, 88, 80, 88, 88, 88, 88, 32, 0, 0, 0, 0, 80,
                                      96, 96, 96, 96, 96, 96, 96, 96, 96, 96, 96, 96, 96, 96, 88, 80 };
static const uint8_t run_rot[32] = { 30, 72, 30, 72, 72, 64, 72, 72, 60, 72, 30, 0, 0, 0, 0, 64,
                                     72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 72, 64 };
#define FACING_SLACK 0x15E    // walk modes turn on the spot until within this (FUN_800622fc)
#define TURN_TO_SLACK 0x80    // mode 0x15 (FUN_80062524)
#define ARRIVED 100           // distance that counts as there (FUN_800623cc)

enum { OP_PLC_MOTION = 0x3F, OP_PLC_DEST = 0x40, OP_PLC_RET = 0x42, OP_PLC_ROT = 0x58,
       OP_PLC_CNT = 0x5B, OP_PLC_STOP = 0x66 };

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

static int table(const PlcBody *b) { return b->table >= 0 && b->table < 32 ? b->table : 16; }

// DS angle from the character to (x, z); facing = (cos, -sin)
static int angle_to(const PlcBody *b, int32_t x, int32_t z)
{
    return (int)(atan2f((float)-(z - b->z), (float)(x - b->x)) * 32768.0f / 6.2831853f) & 32767;
}

static int angle_diff(int want, int dir) { return ((want - dir + 16384) & 32767) - 16384; }

// turn by at most rate RE2 units toward the DS angle want
static void turn_to(PlcBody *b, int want, int rate)
{
    int d = angle_diff(want, b->dir), step = abs(rate) * 8;
    b->dir = (b->dir + (d > step ? step : d < -step ? -step : d)) & 32767;
}

// FUN_80014ef8: toward (x, z), away from it for a negative rate
static void turn_toward(PlcBody *b, int32_t x, int32_t z, int rate)
{
    turn_to(b, (angle_to(b, x, z) + (rate < 0 ? 16384 : 0)) & 32767, rate);
}

static void start_anim(PlcBody *b, const Model *set, int anim)
{
    if (!set || anim < 0 || anim >= set->header->num_anims)
        return;
    b->set = set;
    b->anim = anim;
    b->key = 0;
}

// one keyframe further, looping; footsteps (frame flag 0x4000)
static void loop_anim(PlcBody *b)
{
    const ModelAnim *a = &b->set->anims[b->anim];
    if (!a->count)
        return;
    b->key = (b->key + 1) % a->count;
    if (b->set->anim_pool[a->first + b->key] & ANIM_FLAG_STEP)
        b->footstep = 1;
}

static void forward(PlcBody *b, int speed)
{
    b->x += (speed * cosLerp(b->dir)) >> 12;
    b->z -= (speed * sinLerp(b->dir)) >> 12;
}

void plc_command(Plc *p, PlcBody *b, const uint8_t *ins)
{
    switch (ins[0]) {
    case OP_PLC_MOTION: // scd_op_3f: mode, animation, flags
        p->active = 1;
        p->mode = ins[1];
        p->step = 0;
        p->anim = ins[2];
        p->flags = ins[3];
        p->rot = p->rot_end = 0;
        p->key = 0;
        break;
    case OP_PLC_DEST: { // scd_op_40: pad, mode, arrival bit, x, z
        int mode = ins[2];
        if (!(p->flags & 4) || p->mode != mode || !p->active) {
            p->mode = mode;
            p->step = 0;
        }
        p->active = 1;
        int t = table(b);
        switch (mode) {
        case PLC_WALK: case PLC_WALK_CLIMB: p->rot = walk_rot[t]; break;
        case PLC_RUN: p->rot = run_rot[t]; break;
        case PLC_BACK: p->rot = walk_rot[t]; break;
        case PLC_BACK_STRAIGHT: p->rot = walk_rot[t]; break;
        case PLC_FACE: p->rot = face_rot[t]; break;
        }
        p->flags = 0;
        p->rot_end = 0;
        p->done_bit = ins[3];
        p->dest_x = rd16(ins + 4);
        p->dest_z = rd16(ins + 6);
        break;
    }
    case OP_PLC_RET: // scd_op_42: back to the character's own control
        p->active = 0;
        break;
    case OP_PLC_ROT: // scd_op_58: 0 = per tick, 1 = once at the end of the animation
        if (ins[1] == 0)
            p->rot = rd16(ins + 2);
        else if (ins[1] == 1)
            p->rot_end = rd16(ins + 2);
        break;
    case OP_PLC_CNT: // scd_op_5b: jump to a keyframe
        p->key = ins[1];
        break;
    case OP_PLC_STOP: // scd_op_66: freeze, or go on where it stopped
        if (p->mode == PLC_PAUSED) {
            p->mode = p->saved_mode;
            p->step = p->saved_step;
        } else {
            p->saved_mode = p->mode;
            p->saved_step = p->step;
            p->mode = PLC_PAUSED;
        }
        break;
    }
}

// modes 0 / 1 (FUN_800621b0): play the animation once (looping with flag 4),
// twice as fast with flag 8, backward with flag 0x80; turn rot per tick and
// rot_end at the end; afterwards hold the last pose
static void play(Plc *p, PlcBody *b, const Model *set, int anim)
{
    if (p->step == 0) {
        p->step = 1;
        start_anim(b, set, anim);
        b->no_blend = (p->flags & 0x40) != 0;
    }
    if (p->step != 1 || !set || b->set != set)
        return;
    const ModelAnim *a = &set->anims[b->anim];
    for (int pass = 0; pass < ((p->flags & 8) ? 2 : 1) && p->step == 1; pass++) {
        int k = p->key < a->count ? p->key : a->count - 1;
        b->key = (p->flags & 0x80) ? a->count - 1 - k : k;
        if (set->anim_pool[a->first + b->key] & ANIM_FLAG_STEP)
            b->footstep = 1;
        if (++p->key >= a->count) {
            p->key = 0;
            p->step = (p->flags & 4) ? 1 : 2;
            b->dir = (b->dir + p->rot_end * 8) & 32767;
        }
    }
    b->dir = (b->dir + p->rot * 8) & 32767;
}

static int facing(const PlcBody *b, int32_t x, int32_t z, int slack)
{
    return abs(angle_diff(angle_to(b, x, z), b->dir)) < slack * 8;
}

static void arrived(Plc *p, int keep_mode)
{
    flag_set(5, p->done_bit, 1);
    if (p->flags & 4) {
        p->mode = keep_mode;
        p->step = 2;
    } else {
        p->mode = PLC_STAND;
        p->step = 0;
    }
}

void plc_tick(Plc *p, PlcBody *b)
{
    b->footstep = 0;
    b->no_blend = 0;
    if (!p->active)
        return;
    if (p->flags & 0x10) { // half speed
        p->odd ^= 1;
        if (!p->odd)
            return;
    }
    int t = table(b);
    switch (p->mode) {
    case PLC_ANIM_ROOM:
        play(p, b, b->room_set, p->anim);
        break;
    case PLC_ANIM_OWN:
        play(p, b, b->own_set, b->own_first + p->anim);
        break;
    case PLC_ANIM_SET2:
        play(p, b, b->own_set, b->own_first2 + p->anim);
        break;
    case PLC_ANIM_SET3:
        play(p, b, b->own_set, b->own_first3 + p->anim);
        break;
    case PLC_STAND: // FUN_80061938
        if (p->step == 0) {
            p->step = 1;
            start_anim(b, b->own_set, b->idle_anim);
        }
        loop_anim(b);
        break;
    case PLC_WALK:
    case PLC_WALK_CLIMB:
    case PLC_RUN:
    case PLC_BACK:
    case PLC_BACK_STRAIGHT:
        if (p->step == 0) { // FUN_80061668 / 0x800619c0 / 0x80061a9c / 0x80061d08
            int back = p->mode == PLC_BACK || p->mode == PLC_BACK_STRAIGHT;
            p->speed = back ? -back_speed[t] : walk_speed[t];
            if (p->mode == PLC_RUN)
                p->speed = p->speed * 3 / 2;
            start_anim(b, b->own_set, b->walk_anim);
            p->step = back ? 2 : 1;
        }
        if (p->step == 1) { // turn on the spot first (FUN_800622fc)
            if (!facing(b, p->dest_x, p->dest_z, FACING_SLACK)) {
                turn_toward(b, p->dest_x, p->dest_z, p->rot);
                loop_anim(b);
                break;
            }
            p->step = 2;
        }
        if (p->step == 2) { // walk (FUN_800623cc)
            turn_toward(b, p->dest_x, p->dest_z, p->speed < 0 ? -p->rot : p->rot);
            forward(b, p->speed);
            loop_anim(b);
            float dx = (float)(b->x - p->dest_x), dz = (float)(b->z - p->dest_z);
            if (sqrtf(dx * dx + dz * dz) < ARRIVED)
                arrived(p, p->mode);
        }
        break;
    case PLC_FACE: // FUN_80061b7c: walking on the spot until it faces the point
        if (p->step == 0) {
            start_anim(b, b->own_set, b->walk_anim);
            p->step = 1;
        }
        if (p->step != 1)
            break;
        if (facing(b, p->dest_x, p->dest_z, FACING_SLACK)) {
            arrived(p, PLC_FACE);
            break;
        }
        turn_toward(b, p->dest_x, p->dest_z, p->rot);
        loop_anim(b);
        break;
    case PLC_TURN_TO: { // FUN_80061e8c: dest_x is a facing (RE2 units)
        if (p->step == 0) {
            start_anim(b, b->own_set, b->walk_anim);
            p->step = 1;
        }
        if (p->step != 1)
            break;
        int want = (p->dest_x * 8) & 32767;
        if (abs(angle_diff(want, b->dir)) < TURN_TO_SLACK * 8) {
            arrived(p, PLC_TURN_TO);
            break;
        }
        turn_to(b, want, p->rot ? p->rot : face_rot[t]);
        loop_anim(b);
        break;
    }
    default: // PLC_PAUSED and modes disc 1 does not use: hold still
        break;
    }
}
