// re2ds - room effect sprites (see roomfx.h).
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "roomfx.h"

#define ROOMFX_MAGIC 0x31455252  // "RRE1"
#define MAX_EFFECTS 8
#define MAX_PALETTES 4
#define MAX_INSTANCES 48        // room 101 throws 38 glass shards at once
#define WORLD_PER_TEXEL 20        // as effect.c
#define TOWARD_CAMERA 300
#define TEX_BUDGET (64 * 1024)    // texture memory for the room's effects (enemies need theirs)
// The core effects use two texture pages (CORE01.TIM halves): they share
// them, so at most 64 KB, uploaded when first used

typedef struct {
    uint8_t id, palettes, frames, sprites, subs, log2;
    uint16_t pad;
    uint32_t offset;
} EffectEntry;

typedef struct { uint8_t sprite, count, time, size; } Frame; // time 255: go to frame <sprite>, 0: end
typedef struct { uint8_t u, v; int8_t x, y; } Sprite;
typedef struct { uint8_t first, flags; uint16_t scale, speed, pad; } Sub; // see tools/resp2ds.py
#define SUB_MIRRORED 1
#define SUB_REPEATS 2
#define SUB_FALLS 4                // glass shards: thrown along the direction, fall, vanish on the floor
#define THROWN 8                   // instance flag: roomfx_throw (casings), lies a while after landing
#define LIE_TICKS 60
#define GRAVITY 6                  // as effect.c
// A thrown casing moves by its movement entry (FUN_8001d070: the speed
// +0x0c..0x10 grows by the bytes +8..10 each frame; CORE00.ESP effect 2:
// 0, +10, 0) and bounces on the floor (FUN_8002038c, the entry's floor
// handler 0x2c): back up at 40 % of its speed, on at 60 %; on a wall
// (FUN_8002047c, handler 0x2d) its way along turns round. Each contact plays
// FUN_80057364(u16 +0x12 << 16 | 1). +0x12 is in
// the effect's movement entry (CORE00.ESP effect 2, copied by FUN_8001b8fc):
// 0x0102 for the handgun casing (sub 0), 0x0109 for the shotgun shell (sub 2),
// i.e. bank 1 (the weapon's ARMSnn) sound 2 / 9. Here only the first contact
// plays it, once a shot: the later bounces stay quiet.
#define THROWN_GRAVITY 10
// Movement steps per game tick. By the code above a casing thrown from the
// hand (about 2770 up) lands after some 29 ticks (1.2 s), but in the original
// its clink is heard right after the shot: two steps a tick (as at 50 Hz),
// set by ear against the PS1 game until the cause is found.
#define THROWN_STEPS 2
#define BOUNCE_UP 40
#define BOUNCE_ON 60
#define BOUNCE_MIN 40              // slower than this it stays down (about 3 clicks)

typedef struct {
    const EffectEntry *e;
    const uint16_t *palette;      // palettes x 16 colours
    const Frame *frames;
    const Sprite *sprites;
    const Sub *subs;
    const uint8_t *texels;
    int tex;                      // GL name, 0 = not uploaded
    int pal[MAX_PALETTES];        // palette holders (glAssignColorTable)
    uint8_t failed;
    int8_t owner;                 // core effect whose texture (the same page) this one uses, -1 = own
} Effect;

typedef struct {
    uint8_t active, effect, type, frame, timer, flags, row, first;
    uint8_t id;
    int32_t x, y, z;
    int32_t vx, vy, vz;           // falling sprites
    int32_t floor;                // thrown: lands here
    uint8_t lying;                // thrown: ticks left on the floor
    uint8_t sound;                // thrown: weapon sound on each floor contact
    int32_t scale;                // 4096 = 1
} Instance;

// effects[0 .. num_effects) are the room's, then the core ones (CORE00.ESP)
static uint8_t *data, *core_data;
static Effect effects[2 * MAX_EFFECTS];
static int num_effects, num_core, tex_used;

static int parse(uint8_t *blob, uint32_t size, Effect *out, int max)
{
    if (!blob || size < 8 || *(uint32_t *)blob != ROOMFX_MAGIC)
        return 0;
    int n = *(uint16_t *)(blob + 4);
    if (n > max)
        n = max;
    for (int i = 0; i < n; i++) {
        Effect *f = &out[i];
        memset(f, 0, sizeof(*f));
        f->owner = -1;
        f->e = (const EffectEntry *)(blob + 8) + i;
        const uint8_t *p = blob + f->e->offset;
        f->palette = (const uint16_t *)p;
        p += f->e->palettes * 32;
        f->frames = (const Frame *)p;
        p += f->e->frames * 4;
        f->sprites = (const Sprite *)p;
        p += f->e->sprites * 4;
        f->subs = (const Sub *)p;
        p += f->e->subs * sizeof(Sub);
        p += (4 - ((p - (blob + f->e->offset)) & 3)) & 3;
        f->texels = p;
    }
    DC_FlushRange(blob, size);
    return n;
}
static Instance inst[MAX_INSTANCES];

void roomfx_load(int stage, int room)
{
    uint32_t size;
    if (!core_data) {
        core_data = asset_load("nitro:/effect/core_fx.bin", &size);
        num_core = parse(core_data, size, effects + MAX_EFFECTS, MAX_EFFECTS);
        for (int i = MAX_EFFECTS; i < MAX_EFFECTS + num_core; i++) { // the same page: one texture
            Effect *f = &effects[i];
            for (int j = MAX_EFFECTS; j < i && f->owner < 0; j++)
                if (effects[j].owner < 0 && effects[j].e->log2 == f->e->log2 &&
                    !memcmp(effects[j].texels, f->texels, 128 << f->e->log2))
                    f->owner = j;
        }
    }
    for (int i = MAX_EFFECTS; i < MAX_EFFECTS + num_core; i++) { // textures again per room
        Effect *f = &effects[i];
        if (f->tex && f->owner < 0)
            glDeleteTextures(1, &f->tex);
        for (int r = 0; r < MAX_PALETTES; r++)
            if (f->pal[r])
                glDeleteTextures(1, &f->pal[r]);
        f->tex = 0;
        f->failed = 0;
        memset(f->pal, 0, sizeof(f->pal));
    }
    for (int i = 0; i < num_effects; i++) {
        if (effects[i].tex)
            glDeleteTextures(1, &effects[i].tex);
        for (int r = 0; r < MAX_PALETTES; r++)
            if (effects[i].pal[r])
                glDeleteTextures(1, &effects[i].pal[r]);
    }
    memset(effects, 0, MAX_EFFECTS * sizeof(Effect));
    memset(inst, 0, sizeof(inst));
    num_effects = tex_used = 0;
    asset_free(data);
    data = NULL;

    char path[48];
    snprintf(path, sizeof(path), "nitro:/room/room%d%02x0_esp.bin", stage, room);
    data = asset_load(path, &size);
    num_effects = parse(data, size, effects, MAX_EFFECTS);
    if (!num_effects && data) {
        asset_free(data);
        data = NULL;
    }
}

// texture and palettes on first use, the room's within the budget
static int upload(Effect *f)
{
    if (f->tex || f->failed)
        return f->tex != 0;
    int bytes = 128 << f->e->log2, core = f >= effects + MAX_EFFECTS;
    if (f->owner >= 0) { // the page another core effect has uploaded
        Effect *o = &effects[f->owner];
        if (!upload(o)) {
            f->failed = 1;
            return 0;
        }
        f->tex = o->tex;
        goto palettes;
    }
    if (!core && tex_used + bytes > TEX_BUDGET) {
        f->failed = 1;
        return 0;
    }
    glGenTextures(1, &f->tex);
    glBindTexture(0, f->tex);
    if (!glTexImage2D(0, 0, GL_RGB16, TEXTURE_SIZE_256, f->e->log2 - 3, 0,
                      TEXGEN_TEXCOORD | GL_TEXTURE_COLOR0_TRANSPARENT, f->texels)) {
        glDeleteTextures(1, &f->tex);
        f->tex = 0;
        f->failed = 1;
        return 0;
    }
    if (!core)
        tex_used += bytes;
palettes:;
    int rows = f->e->palettes < MAX_PALETTES ? f->e->palettes : MAX_PALETTES;
    for (int r = 0; r < rows; r++) {
        glGenTextures(1, &f->pal[r]);
        glBindTexture(0, f->pal[r]);
        glColorTableEXT(0, 0, 16, 0, 0, f->palette + 16 * r);
    }
    glBindTexture(0, f->tex);
    glAssignColorTable(0, f->pal[0]);
    return 1;
}

// the room's effect with this id, else the core one
static Effect *find(int id)
{
    for (int i = 0; i < num_effects; i++)
        if (effects[i].e->id == id)
            return &effects[i];
    for (int i = MAX_EFFECTS; i < MAX_EFFECTS + num_core; i++)
        if (effects[i].e->id == id)
            return &effects[i];
    return NULL;
}

static int16_t rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }

void roomfx_spawn(int id, int type, int32_t x, int32_t y, int32_t z, int size, int dir_re2)
{
    Effect *f = find(id);
    if (!f || !upload(f))
        return;
    int sub = type & 7;
    if (sub >= f->e->subs)
        sub = 0;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        Instance *n = &inst[i];
        if (n->active)
            continue;
        memset(n, 0, sizeof(*n));
        n->active = 1;
        n->id = id;
        n->effect = f - effects;
        n->type = type;
        n->row = type >> 3;
        n->first = f->e->subs ? f->subs[sub].first : 0;
        n->flags = f->e->subs ? f->subs[sub].flags : 0;
        n->frame = n->first;
        n->timer = f->frames[n->frame].time;
        n->x = x;
        n->y = y;
        n->z = z;
        if (n->flags & SUB_FALLS) { // speed along the direction (RE2 units; facing (cos, -sin))
            int speed = f->subs[sub].speed / 4, a = dir_re2 * 8;
            n->vx = (speed * cosLerp(a)) >> 12;
            n->vz = -((speed * sinLerp(a)) >> 12);
        }
        int sub_scale = f->e->subs && f->subs[sub].scale ? f->subs[sub].scale : 4096;
        n->scale = (size ? size * 64 : 4096) * sub_scale / 4096;
        return;
    }
}

void roomfx_throw(int id, int first, int row, int32_t x, int32_t y, int32_t z, int32_t vx, int32_t vy, int32_t vz,
                  int32_t floor_y, int scale, int sound)
{
    Effect *f = find(id);
    if (!f || !upload(f) || first >= f->e->frames)
        return;
    for (int i = 0; i < MAX_INSTANCES; i++) {
        Instance *n = &inst[i];
        if (n->active)
            continue;
        memset(n, 0, sizeof(*n));
        n->active = 1;
        n->id = id;
        n->effect = f - effects;
        n->type = row << 3;
        n->row = row;
        n->first = n->frame = first;
        n->flags = THROWN;
        n->sound = sound;
        n->timer = f->frames[first].time;
        n->x = x;
        n->y = y;
        n->z = z;
        n->vx = vx;
        n->vy = vy;
        n->vz = vz;
        n->floor = floor_y;
        n->scale = scale;
        return;
    }
}

void roomfx_on(const uint8_t *ins)
{
    // op, ?, id, sub | row << 3, object type, object index, u16 parameter,
    // s16 x, y, z, direction; the parameter's high byte looks like a size
    // (0x24..0x40 for the fires of room 100)
    roomfx_spawn(ins[2], ins[3], rd16(ins + 8), rd16(ins + 10), rd16(ins + 12), ins[7], rd16(ins + 14));
}

void roomfx_kill(int id, int type)
{
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (inst[i].active && inst[i].id == id && inst[i].type == type)
            inst[i].active = 0;
}

// One movement step of a thrown casing; 1 on a floor contact.
static int thrown_step(Instance *n)
{
    if (game_blocked(n->x, n->z, n->x + n->vx, n->z + n->vz, n->floor)) {
        n->vx = -n->vx;
        n->vz = -n->vz;
    }
    n->x += n->vx;
    n->y += n->vy;
    n->z += n->vz;
    n->vy += THROWN_GRAVITY;
    if (n->y < n->floor)
        return 0;
    n->y = n->floor;
    if (n->sound) {
        game_casing_sound(n->sound);
        n->sound = 0;
    }
    if (n->vy > BOUNCE_MIN) {
        n->vy = -n->vy * BOUNCE_UP / 100;
        n->vx = n->vx * BOUNCE_ON / 100;
        n->vz = n->vz * BOUNCE_ON / 100;
    } else {
        n->lying = LIE_TICKS;
    }
    return 1;
}

void roomfx_tick(void)
{
    for (int i = 0; i < MAX_INSTANCES; i++) {
        Instance *n = &inst[i];
        if (!n->active)
            continue;
        const Effect *f = &effects[n->effect];
        if (n->flags & THROWN) {
            if (n->lying) { // on the floor: still, then gone
                if (--n->lying == 0)
                    n->active = 0;
                continue;
            }
            int landed = 0;
            for (int k = 0; k < THROWN_STEPS && !n->lying; k++)
                landed |= thrown_step(n);
            if (landed)
                continue;
        }
        if (n->flags & SUB_FALLS) {
            n->x += n->vx;
            n->y += n->vy;
            n->z += n->vz;
            n->vy += GRAVITY;
            if (n->y >= 0) { // the floor
                n->active = 0;
                continue;
            }
        }
        if (n->timer > 1) {
            n->timer--;
            continue;
        }
        int next = n->frame + 1;
        for (int guard = 0; guard < 4; guard++) {
            if (next >= f->e->frames || f->frames[next].time == 0) { // the end
                if (!(n->flags & SUB_REPEATS)) {
                    n->active = 0;
                    break;
                }
                next = n->first;
                continue;
            }
            if (f->frames[next].time != 255)
                break;
            next = f->frames[next].sprite; // go to
        }
        if (!n->active)
            continue;
        n->frame = next;
        n->timer = f->frames[next].time;
    }
}

// quads: the sprites of every shown frame
int roomfx_count(void)
{
    int c = 0;
    for (int i = 0; i < MAX_INSTANCES; i++)
        if (inst[i].active)
            c += effects[inst[i].effect].frames[inst[i].frame].count;
    return c;
}

void roomfx_draw(const int32_t cam_from[3], const int32_t cam_to[3])
{
    if (!roomfx_count())
        return;
    float f[3], r[3], u[3], len;
    for (int k = 0; k < 3; k++)
        f[k] = (float)(cam_to[k] - cam_from[k]);
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    int32_t nearer[3];
    for (int k = 0; k < 3; k++)
        nearer[k] = (int32_t)(-f[k] / len * TOWARD_CAMERA);
    r[0] = f[2];
    r[1] = 0.0f;
    r[2] = -f[0];
    len = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= len;
    r[2] /= len;
    // screen down on the billboard = world +Y tilted with the camera
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int k = 0; k < 3; k++)
        u[k] /= len;
    if (u[1] < 0) // keep "down" pointing down (+Y)
        for (int k = 0; k < 3; k++)
            u[k] = -u[k];

    glPolyFmt(POLY_ALPHA(28) | POLY_CULL_NONE | POLY_ID(61));
    glColor3b(255, 255, 255);
    for (int i = 0; i < MAX_INSTANCES; i++) {
        const Instance *n = &inst[i];
        if (!n->active)
            continue;
        const Effect *fx = &effects[n->effect];
        const Frame *fr = &fx->frames[n->frame];
        glBindTexture(0, fx->tex);
        if (n->row < MAX_PALETTES && fx->pal[n->row])
            glAssignColorTable(0, fx->pal[n->row]);
        else
            glAssignColorTable(0, fx->pal[0]);
        glBindTexture(0, fx->tex);
        float k = WORLD_PER_TEXEL * n->scale / 4096.0f;
        glPushMatrix();
        glTranslatef32(n->x + nearer[0], n->y + nearer[1], n->z + nearer[2]);
        for (int s = fr->sprite; s < fr->sprite + fr->count && s < fx->e->sprites; s++) {
        const Sprite *sp = &fx->sprites[s];
        // corners in texels from the anchor: x right, y down
        float x0 = sp->x, x1 = sp->x + fr->size, y0 = sp->y, y1 = sp->y + fr->size;
        int u0 = sp->u, u1 = sp->u + fr->size;
        if (n->flags & SUB_MIRRORED) {
            float t = -x1;
            x1 = -x0;
            x0 = t;
            int tu = u0;
            u0 = u1;
            u1 = tu;
        }
        float xs[4] = { x0, x1, x1, x0 }, ys[4] = { y0, y0, y1, y1 };
        int us[4] = { u0, u1, u1, u0 }, vs[4] = { sp->v, sp->v, sp->v + fr->size, sp->v + fr->size };
        glBegin(GL_QUADS);
        for (int c = 0; c < 4; c++) {
            glTexCoord2t16(inttot16(us[c]), inttot16(vs[c]));
            glVertex3v16((int16_t)((r[0] * xs[c] + u[0] * ys[c]) * k), (int16_t)((u[1] * ys[c]) * k),
                         (int16_t)((r[2] * xs[c] + u[2] * ys[c]) * k));
        }
        glEnd();
        }
        glPopMatrix(1);
    }
}
