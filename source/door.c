// re2ds - door sequence (see door.h).
//
// The door program is SCD bytecode with the normal opcode table. Up to four
// script objects (ids 10..13) run it; object 10 starts at function 0 and the
// sequence ends when it finishes. The commands that matter here:
//   work_set 05 n     select door part n, clear speeds (scd_op_2e)
//   speed_set i v     speed i: 0-2 move x/y/z, 3-5 turn x/y/z, 6-11 their
//                     accelerations (scd_op_2f)
//   add_speed         part position += move, rotation += turn (scd_op_30)
//   add_aspeed        speeds += accelerations (scd_op_31)
//   member_set(2)/member_copy  part fields: 0x0B-0x0D position (0x8005202c)
//   door_model_set    place model part (0x800146bc); flag 0x10 = attached to
//                     part flags & 15, 0x800 = play the closing sound at the end
//   se_on             door sound (0 = opens)
//   sce_fade_set/0x74 screen fade speed / level
// Camera: eye (10000, 0, 0) looking at the origin (0x80013734), screen
// distance 290 (FUN_800892f0(0x122)). The task runs every display frame
// (VSync(0)), i.e. 50 Hz on PAL.
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "door.h"
#include "script.h"
#include "sound.h"

#define DOOR_HZ 50
#define DS_HZ 60
#define MAX_PARTS 8
#define NUM_THREADS 4
#define FIRST_ID 10
#define MAX_FRAMES 1500     // safety net: 30 s
#define FADE_BLACK 0x7FFF

enum { B_IF, B_FOR, B_WHILE, B_SWITCH };

typedef struct {
    uint8_t active, part;
    uint16_t flags;
    int32_t pos[3];
    int16_t rot[3];         // RE2 units, 4096 = 360 degrees
} Part;

typedef struct {
    uint8_t type;
    uint16_t start, end, loop, count;
} Block;

typedef struct {
    uint8_t active;
    uint16_t pc, sleep;
    int nblocks, ncalls;
    Block blocks[8];
    struct {
        uint16_t pc;
        uint8_t nblocks;
    } calls[4];
    int work;               // selected part, -1 = none
    int16_t spd[12];
} DoorThread;

void setup_camera(const int32_t from[3], const int32_t to[3], int screen); // main.c

static Model model;
static uint8_t *tex_data;
static int tex_id = -1;
static uint8_t *prog;
static void *bank;
static Part parts[MAX_PARTS];
static DoorThread threads[NUM_THREADS];
static int16_t vars[32];
static int fade_level, fade_speed;
static int tick_acc, frames, close_sound;

static inline int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static inline uint16_t ru16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t *var(int n) { return &vars[n & 31]; }

static uint16_t func_start(int n)
{
    uint16_t table = ru16(prog);
    return 2 * n < table ? ru16(prog + 2 * n) : 0xFFFF;
}

static void start_thread(int slot, int func)
{
    uint16_t pc = func_start(func);
    if (slot < 0 || slot >= NUM_THREADS || pc == 0xFFFF)
        return;
    DoorThread *t = &threads[slot];
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->pc = pc;
    t->work = -1;
}

static int base(const DoorThread *t) { return t->ncalls ? t->calls[t->ncalls - 1].nblocks : 0; }

static void push(DoorThread *t, int type, int start, int end, int loop, int count)
{
    if (t->nblocks < 8)
        t->blocks[t->nblocks++] = (Block){ type, start, end, loop, count };
}

static void unwind(DoorThread *t)
{
    while (t->nblocks > base(t)) {
        Block *b = &t->blocks[t->nblocks - 1];
        if (t->pc >= b->start && t->pc < b->end)
            return;
        t->nblocks--;
    }
}

static void condition_failed(DoorThread *t)
{
    for (int i = t->nblocks - 1; i >= base(t); i--) {
        Block *b = &t->blocks[i];
        if (b->type == B_IF) {
            t->pc = b->end;
            if (b->end >= 4 && prog[b->end - 4] == 0x07) { // into the else body
                b->end = b->end - 4 + ru16(prog + b->end - 2);
                t->nblocks = i + 1;
            } else {
                t->nblocks = i;
            }
            return;
        }
        if (b->type == B_WHILE) {
            t->pc = b->end;
            t->nblocks = i;
            return;
        }
    }
}

static int compare(int16_t a, int op, int16_t b)
{
    switch (op) {
    case 0: return a == b;
    case 1: return a > b;
    case 2: return a >= b;
    case 3: return a < b;
    case 4: return a <= b;
    case 5: return a != b;
    default: return (a & b) != 0;
    }
}

static int16_t calc(int16_t a, int op, int16_t b)
{
    switch (op) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a * b;
    case 3: return b ? a / b : 0;
    case 4: return b ? a % b : 0;
    case 5: return a | b;
    case 6: return a & b;
    case 7: return a ^ b;
    default: return a;
    }
}

static Part *work_part(const DoorThread *t)
{
    return t->work >= 0 && t->work < MAX_PARTS ? &parts[t->work] : NULL;
}

// part fields as the PS1 member table addresses them (0x8005202c)
static void member_store(Part *p, int member, int value)
{
    if (!p)
        return;
    if (member == 0)
        p->active = value != 0;
    else if (member >= 0x0B && member <= 0x0D)
        p->pos[member - 0x0B] = value;
    else if (member >= 0x0E && member <= 0x10)
        p->rot[member - 0x0E] = value;
}

static int member_load(const Part *p, int member)
{
    if (!p)
        return 0;
    if (member >= 0x0B && member <= 0x0D)
        return p->pos[member - 0x0B];
    if (member >= 0x0E && member <= 0x10)
        return p->rot[member - 0x0E];
    return member == 0 ? p->active : 0;
}

static void run_thread(DoorThread *t)
{
    if (t->sleep) {
        t->sleep--;
        return;
    }
    for (int budget = 1000; budget > 0 && t->active; budget--) {
        unwind(t);
        const uint8_t *ins = prog + t->pc;
        int op = ins[0], size = scd_op_size[op];
        if (!size) {
            t->active = 0;
            return;
        }
        int next = t->pc + size, cond = -1;
        Part *p = work_part(t);
        switch (op) {
        case 0x01:
        case 0x19:
            if (!t->ncalls) {
                t->active = 0;
                return;
            }
            t->ncalls--;
            next = t->calls[t->ncalls].pc;
            t->nblocks = t->calls[t->ncalls].nblocks;
            break;
        case 0x02:
            t->pc = next;
            return;
        case 0x04:
            start_thread(ins[1] - FIRST_ID, ins[3]);
            break;
        case 0x06:
            push(t, B_IF, t->pc, next + ru16(ins + 2), 0, 0);
            break;
        case 0x07:
            next = t->pc + ru16(ins + 2);
            break;
        case 0x09:
            t->sleep = ru16(ins + 2);
            t->pc = next;
            return;
        case 0x0D:
            if (ru16(ins + 4) == 0)
                next += ru16(ins + 2);
            else
                push(t, B_FOR, t->pc, next + ru16(ins + 2), next, ru16(ins + 4));
            break;
        case 0x0E:
            if (t->nblocks > base(t)) {
                Block *b = &t->blocks[t->nblocks - 1];
                if (b->type == B_FOR && --b->count > 0)
                    next = b->loop;
            }
            break;
        case 0x0F:
            push(t, B_WHILE, t->pc, next + ru16(ins + 2), 0, 0);
            break;
        case 0x10:
            if (t->nblocks > base(t) && t->blocks[t->nblocks - 1].type == B_WHILE) {
                t->pc = t->blocks[--t->nblocks].start;
                return;
            }
            break;
        case 0x13: { // switch
            uint16_t end = next + ru16(ins + 2), q = next, target = end;
            int16_t value = *var(ins[1]);
            while (q < end) {
                if (prog[q] == 0x14) {
                    if (rd16(prog + q + 4) == value) {
                        target = q + 6;
                        break;
                    }
                    q += 6 + ru16(prog + q + 2);
                } else if (prog[q] == 0x15) {
                    target = q + 2;
                    break;
                } else {
                    break;
                }
            }
            push(t, B_SWITCH, t->pc, end, 0, 0);
            next = target;
            break;
        }
        case 0x17:
            next = t->pc + rd16(ins + 4);
            break;
        case 0x18: {
            uint16_t pc = func_start(ins[1]);
            if (pc != 0xFFFF && t->ncalls < 4) {
                t->calls[t->ncalls].pc = next;
                t->calls[t->ncalls].nblocks = t->nblocks;
                t->ncalls++;
                next = pc;
            }
            break;
        }
        case 0x1A:
            for (int i = t->nblocks - 1; i >= base(t); i--)
                if (t->blocks[i].type != B_IF) {
                    next = t->blocks[i].end;
                    t->nblocks = i;
                    break;
                }
            break;
        case 0x23:
            cond = compare(*var(ins[2]), ins[3], rd16(ins + 4));
            break;
        case 0x24:
            *var(ins[1]) = rd16(ins + 2);
            break;
        case 0x25:
            *var(ins[1]) = *var(ins[2]);
            break;
        case 0x26:
            *var(ins[3]) = calc(*var(ins[3]), ins[2], rd16(ins + 4));
            break;
        case 0x2E:
            memset(t->spd, 0, sizeof(t->spd));
            t->work = ins[1] == 5 ? ins[2] : -1;
            break;
        case 0x2F:
            if (ins[1] < 12)
                t->spd[ins[1]] = rd16(ins + 2);
            break;
        case 0x30:
            if (p) {
                for (int k = 0; k < 3; k++) {
                    p->pos[k] += t->spd[k];
                    p->rot[k] += t->spd[3 + k];
                }
            }
            break;
        case 0x31:
            for (int k = 0; k < 6; k++)
                t->spd[k] += t->spd[6 + k];
            break;
        case 0x34:
            member_store(p, ins[1], rd16(ins + 2));
            break;
        case 0x35:
            member_store(p, ins[1], *var(ins[2]));
            break;
        case 0x3D:
            *var(ins[1]) = member_load(p, ins[2]);
            break;
        case 0x36:
            sound_play(bank, rd16(ins + 2) & 0xFF, 127, 64);
            break;
        case 0x4D: { // door_model_set
            if (ins[1] >= MAX_PARTS)
                break;
            Part *q = &parts[ins[1]];
            q->active = ins[4];
            q->part = ins[5];
            q->flags = ru16(ins + 6);
            for (int k = 0; k < 3; k++) {
                q->pos[k] = rd16(ins + 10 + 2 * k);
                q->rot[k] = rd16(ins + 16 + 2 * k);
            }
            if (q->flags & 0x800)
                close_sound = 1;
            break;
        }
        case 0x53:
            fade_speed = rd16(ins + 4);
            break;
        case 0x74:
            fade_level = ru16(ins + 2);
            break;
        default:
            break;
        }
        t->pc = next;
        if (cond == 0)
            condition_failed(t);
    }
}

int door_begin(int file, int variant_byte)
{
    char path[48];
    if (bank) {
        sound_bank_free(bank); // previous door's bank, long done playing
        bank = NULL;
    }
    memset(parts, 0, sizeof(parts));
    memset(threads, 0, sizeof(threads));
    memset(vars, 0, sizeof(vars));
    vars[0x0C] = variant_byte & 0x7F;  // door variant (0x80013734: DAT_800cc400)
    vars[0x0D] = 0;                    // room loading: done before the sequence
    vars[0x0E] = variant_byte & 0x80;
    vars[0x0F] = file;
    fade_level = FADE_BLACK;
    fade_speed = 0;
    tick_acc = frames = close_sound = 0;

    snprintf(path, sizeof(path), "nitro:/door/door%02x_prog.bin", file);
    prog = asset_load(path, NULL);
    snprintf(path, sizeof(path), "nitro:/door/door%02x_model.bin", file);
    if (!prog || !model_load(&model, path)) {
        door_end();
        return 0;
    }
    snprintf(path, sizeof(path), "nitro:/door/door%02x_tex.bin", file);
    uint32_t size;
    tex_data = asset_load(path, &size);
    if (tex_data) {
        DC_FlushRange(tex_data, size);
        glGenTextures(1, &tex_id);
        glBindTexture(0, tex_id);
        glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_256, 0, TEXGEN_TEXCOORD, tex_data + 512);
        glColorTableEXT(0, 0, 256, 0, 0, (const uint16_t *)tex_data);
    }
    snprintf(path, sizeof(path), "nitro:/sound/door%02x.bin", file);
    bank = sound_bank_load(path);
    start_thread(0, 0);
    return 1;
}

static void apply_part(const Part *p, int depth)
{
    if ((p->flags & 0x10) && depth < 4)
        apply_part(&parts[p->flags & 0xF], depth + 1);
    glTranslatef32(p->pos[0], p->pos[1], p->pos[2]);
    glRotateXi(p->rot[0] * 8);
    glRotateYi(p->rot[1] * 8);
    glRotateZi(p->rot[2] * 8);
}

static void render(void)
{
    static const int32_t eye[3] = { 10000, 0, 0 }, at[3] = { 0, 0, 0 };
    setup_camera(eye, at, 290);
    glColor3b(255, 255, 255);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
    if (tex_id >= 0)
        glBindTexture(0, tex_id);
    for (int i = 0; i < MAX_PARTS; i++) {
        const Part *p = &parts[i];
        if (!p->active || p->part >= model.header->num_bones || !model.lists[p->part])
            continue;
        glPushMatrix();
        apply_part(p, 0);
        glCallList((const u32 *)((const uint8_t *)model.data + model.lists[p->part]));
        glPopMatrix(1);
    }
    glFlush(0);
    setBrightness(1, -16 * fade_level / FADE_BLACK);
}

int door_frame(void)
{
    for (tick_acc += DOOR_HZ; tick_acc >= DS_HZ; tick_acc -= DS_HZ) {
        for (int i = 0; i < NUM_THREADS; i++)
            if (threads[i].active)
                run_thread(&threads[i]);
        fade_level += fade_speed;
        if (fade_level < 0)
            fade_level = 0;
        if (fade_level > FADE_BLACK)
            fade_level = FADE_BLACK;
        frames++;
    }
    render();
    return threads[0].active && frames < MAX_FRAMES;
}

void door_end(void)
{
    if (close_sound)
        sound_play(bank, 1, 127, 64);   // FUN_80013c94
    if (tex_id >= 0) {
        glDeleteTextures(1, &tex_id);
        tex_id = -1;
    }
    free(tex_data);
    tex_data = NULL;
    free(prog);
    prog = NULL;
    if (model.data)
        asset_free(model.data);
    model.data = NULL;
    // the bank stays until the next door: the closing sound may still play
}
