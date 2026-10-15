// re2ds - enemies (see enemy.h).
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "enemy.h"

#define MAX_TYPES 4             // models loaded at the same time (texture memory)
#define MAX_PAGES 4
#define ENEMY_RADIUS 400
#define CONTACT_DISTANCE 800    // ENEMY_RADIUS + the player's radius (main.c)
#define MAX_SETS 4

// Zombie behaviour after the PS1 code (zombie overlay of CDEMD0.EMS, linked at
// 0x80100000; re2ds-notes/enemy-zombie.md). States as in obj+5; only the ones
// that need no attacks yet. One game tick here is one PS1 frame.
#define ST_LURK 0               // stands, shuffles now and then (0x801011fc / 0x8010135c)
#define ST_CHASE 1              // shuffles toward Leon (0x8010167c / 0x801019d0)
#define ST_LYING 7              // lies until EF_HOLD goes, then gets up (0x801036b4)
#define ST_EAT 8                // eats (variant 8)

// Enemy.flags = sce_em_set bytes 4..5 (obj+0x10e): spawn variant | status << 8.
// Room scripts read and write it as member 7.
#define EF_VARIANT 0x003F       // 0/3 lurk, 1/6 chase, 2/4 lying, 5/7 lying as dead, 8 eat
#define EF_HOLD 0x4000          // lying: stay down until the room script clears it

// Animations: set 0 holds the gaits, set 1 everything else (emXX_sets.bin)
#define SET_GAIT 0
#define SET_MAIN 1
#define A_STAND 0x00
#define A_GET_UP 0x08           // 0x09 from the other lying pose
#define A_EAT 0x12
#define A_LIE 0x17              // 0x16 = the other lying pose

#define EYE_ENEMY (-2768)       // part 8 above the feet (line of sight, 0x80046fb0)
#define EYE_PLAYER (-2507)

// The DS geometry engine takes at most 6144 vertices (and 2048 polygons) per
// frame and silently drops the rest, so enemies would flicker. Enemies are
// drawn nearest first while they fit; Leon and the masks are counted first.
// Each enemy is planned with what it cost when last drawn (measured with the
// GX RAM counters; it changes a little as the enemy turns or nears the screen
// edge, so keep some headroom).
#define VERTEX_LIMIT 5900
#define POLYGON_LIMIT 1980
#define KEEP_BONUS 1500         // drawn last frame: counts as this much nearer

// collision with the room, shared with the player (main.c)
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y);
// does a room box that blocks sight lie between the two points? (main.c)
int game_sight_blocked(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1);

// Turning while chasing from afar (0x8010c748): frames to keep a course, bit
// 15 = turn away from Leon for that long. The zombies weave toward him.
static const uint16_t wander[32] = {
    0x00be, 0x8032, 0x0096, 0x8040, 0x006e, 0x8040, 0x00b4, 0x8034,
    0x008c, 0x8034, 0x0078, 0x803a, 0x0096, 0x804e, 0x008c, 0x8032,
    0x0096, 0x803e, 0x0096, 0x8048, 0x006e, 0x8046, 0x0078, 0x804a,
    0x00b4, 0x8022, 0x006e, 0x8034, 0x00c8, 0x8042, 0x00b4, 0x8042,
};
// gait (set 0 animation) per zombie, picked at random (0x80100004)
static const uint8_t gaits[8] = { 0, 2, 0, 2, 0, 2, 2, 0 };

typedef struct {
    int type;                   // 0 = free
    Model model;
    uint8_t *tex_data;
    int tex[MAX_PAGES];
    int pages;
    int drawable;               // textures uploaded
    int vertices, polygons;     // what one instance sends to the GPU
    int num_sets;
    uint16_t set_start[MAX_SETS]; // first animation of each set
    int8_t first_child[64], next_sibling[64];
} EnemyType;

typedef struct {
    uint8_t active;
    uint8_t drawn;              // drawn last frame (keeps the choice stable)
    int16_t cost_v, cost_p;     // GX vertices / polygons it took when last drawn
    uint8_t was_visible, was_drawn; // diagnostics
    int8_t type;                // index into types[]
    int32_t x, y, z;
    int16_t dir;                // DS units
    int anim, key;
    uint16_t flags;             // EF_*
    uint8_t state, sub;         // ST_*, step within the state
    uint8_t gait;               // set 0 animation it walks with
    uint8_t lie_alt;            // lies in the other pose (obj+0x21a bit 2)
    uint8_t sees;               // line of sight to Leon (obj+0x154 bit 0x800)
    uint8_t sight_timer;        // the PS1 checks it every 8 frames
    uint8_t wander_idx, turn_timer, standing;
    int16_t timer, timer2;      // obj+0x158, obj+0x15a
} Enemy;

static EnemyType types[MAX_TYPES];
static Enemy enemies[MAX_ENEMIES];

// diagnostics for the debug screen
int enemy_jumps, enemy_jump_max, enemy_toggles; // moves > 200 per tick, largest, draw on/off while visible
int enemy_spawn_calls;
int enemy_last_error;   // 0 ok, 1 model file, 2 model format, 3 no type slot, 4 no anims,
                        // 5 bad slot, 6 texture file, 7 texture memory

static void free_type(EnemyType *t)
{
    for (int p = 0; p < t->pages; p++)
        if (t->tex[p])
            glDeleteTextures(1, &t->tex[p]);
    if (t->model.data)
        asset_free(t->model.data);
    free(t->tex_data);
    memset(t, 0, sizeof(*t));
}

void enemy_room_reset(void)
{
    for (int i = 0; i < MAX_TYPES; i++)
        if (types[i].type)
            free_type(&types[i]);
    memset(enemies, 0, sizeof(enemies));
}

// Vertices and polygons in all display lists of a model (packed GX lists from
// tools/model2ds.py: BEGIN triangles or quads, TEXCOORD, VTX_16, END).
void model_gx_cost(const Model *m, int *vertices, int *polygons)
{
    int verts = 0, polys4 = 0, lists = m->header->num_bones * m->header->num_pages;
    for (int i = 0; i < lists; i++) {
        if (!m->lists[i])
            continue;
        const uint32_t *w = (const uint32_t *)((const uint8_t *)m->data + m->lists[i]);
        uint32_t n = *w++, k = 0;
        int per_poly = 3;
        while (k < n) {
            uint32_t ids = w[k++];
            for (int c = 0; c < 4; c++) {
                int cmd = (ids >> (8 * c)) & 0xFF;
                if (cmd == 0x23) {
                    verts++;
                    polys4 += per_poly == 3 ? 4 : 3; // 12 / per_poly per vertex
                    k += 2;
                } else if (cmd == 0x40) {
                    per_poly = w[k] == 1 ? 4 : 3;
                    k += 1;
                } else if (cmd == 0x20 || cmd == 0x22) {
                    k += 1;
                }
            }
        }
    }
    *vertices = verts;
    *polygons = polys4 / 12;
}

static int load_type(int type)
{
    for (int i = 0; i < MAX_TYPES; i++)
        if (types[i].type == type)
            return i;
    int slot = -1;
    for (int i = 0; i < MAX_TYPES && slot < 0; i++)
        if (!types[i].type)
            slot = i;
    if (slot < 0) {
        enemy_last_error = 3;
        return -1;
    }
    EnemyType *t = &types[slot];
    char path[48];
    snprintf(path, sizeof(path), "nitro:/model/em%02x.bin", type);
    if (!model_load(&t->model, path)) {
        enemy_last_error = t->model.data ? 2 : 1;
        if (t->model.data)
            asset_free(t->model.data);
        t->model.data = NULL;
        return -1;
    }
    t->type = type;
    t->pages = t->model.header->num_pages;
    int n = t->model.header->num_bones;
    memset(t->first_child, -1, sizeof(t->first_child));
    memset(t->next_sibling, -1, sizeof(t->next_sibling));
    for (int b = n - 1; b >= 0; b--) {
        int parent = t->model.bones[b].parent;
        if (parent >= 0 && parent < 64) {
            t->next_sibling[b] = t->first_child[parent];
            t->first_child[parent] = b;
        }
    }

    model_gx_cost(&t->model, &t->vertices, &t->polygons);

    // animation sets: u8 count, u16 first animation per set (tools/em2ds.py)
    uint32_t sets_size;
    snprintf(path, sizeof(path), "nitro:/model/em%02x_sets.bin", type);
    uint8_t *sets = asset_load(path, &sets_size);
    if (sets && sets_size >= 1) {
        t->num_sets = sets[0] < MAX_SETS ? sets[0] : MAX_SETS;
        for (int k = 0; k < t->num_sets && 3 + 2 * k <= (int)sets_size; k++)
            t->set_start[k] = sets[1 + 2 * k] | sets[2 + 2 * k] << 8;
    }
    asset_free(sets);

    uint32_t size;
    snprintf(path, sizeof(path), "nitro:/model/em%02x_tex.bin", type);
    t->tex_data = asset_load(path, &size);
    uint32_t page_size = 512 + TEX_PAGE_WIDTH * TEX_PAGE_HEIGHT;
    if (t->tex_data && t->pages <= MAX_PAGES && size == page_size * t->pages) {
        DC_FlushRange(t->tex_data, size);
        glGenTextures(t->pages, t->tex);
        t->drawable = 1;
        for (int p = 0; p < t->pages; p++) {
            const uint8_t *page = t->tex_data + p * page_size;
            glBindTexture(0, t->tex[p]);
            if (!glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_256, 0, TEXGEN_TEXCOORD,
                              page + 512)) {
                t->drawable = 0; // texture memory full
                enemy_last_error = 7;
            }
            glColorTableEXT(0, 0, 256, 0, 0, (const uint16_t *)page);
        }
    } else {
        enemy_last_error = 6;
    }
    free(t->tex_data); // uploaded: VRAM holds the texture now
    t->tex_data = NULL;
    return slot;
}

static int rnd(void) { return rand() & 0xFF; } // the PS1 generator gives 0..255

static void set_anim(Enemy *e, int set, int n, int key)
{
    const EnemyType *t = &types[e->type];
    int a = (set < t->num_sets ? t->set_start[set] : 0) + n;
    if (a >= t->model.header->num_anims)
        a = 0;
    e->anim = a;
    int count = t->model.anims[a].count;
    e->key = count ? key % count : 0;
}

// Starts a state (what the PS1 does in step 0 of it).
static void enter_state(Enemy *e, int state)
{
    e->state = state;
    e->sub = 1;
    e->standing = 0;
    switch (state) {
    case ST_LURK:
        set_anim(e, SET_MAIN, A_STAND, rnd() & 0x1F);
        e->timer = rnd() + 150;
        e->timer2 = rnd() + 300;
        break;
    case ST_CHASE:
        set_anim(e, SET_GAIT, e->gait, rnd() & 0x1F);
        e->wander_idx = (rnd() & 0xF) << 1;
        e->timer = (wander[e->wander_idx] & 0x7FFF) + rnd();
        e->turn_timer = (rnd() & 0x1F) + 30;
        break;
    case ST_LYING:
        set_anim(e, SET_MAIN, e->lie_alt ? A_LIE - 1 : A_LIE, 0);
        break;
    case ST_EAT:
        set_anim(e, SET_MAIN, A_EAT, 0);
        break;
    }
}

void enemy_spawn(int slot, int type, int flags, int32_t x, int32_t y, int32_t z, int16_t dir_re2)
{
    enemy_spawn_calls++;
    if (slot < 0 || slot >= MAX_ENEMIES) {
        enemy_last_error = 5;
        return;
    }
    int t = load_type(type);
    if (t < 0)
        return;
    Enemy *e = &enemies[slot];
    memset(e, 0, sizeof(*e));
    e->active = 1;
    e->type = t;
    e->x = x;
    e->y = y;
    e->z = z;
    e->dir = (dir_re2 * 8) & 32767;
    if (types[t].model.header->num_anims == 0) {
        e->active = 0;
        enemy_last_error = 4;
        return;
    }
    e->flags = flags;
    e->gait = gaits[(rnd() >> (rnd() & 3)) & 7];
    e->sight_timer = slot & 7;

    // start state by spawn variant (zombie init 0x80100674)
    int state = ST_LURK;
    switch (flags & EF_VARIANT) {
    case 2: e->lie_alt = 1; state = ST_LYING; break;
    case 4: state = ST_LYING; break;
    case 5: e->lie_alt = 1; e->flags = EF_HOLD | 2; state = ST_LYING; break; // lies as if dead
    case 7: e->flags = EF_HOLD | 4; state = ST_LYING; break;
    case 1:                                    // PS1: lunges at once (state 2, not done yet)
    case 6: state = ST_CHASE; break;
    case 8: state = ST_EAT; break;
    }
    enter_state(e, state);
}

// Members the room scripts read (FUN_8005202c lists all): 0 = object flags,
// bit 0 set while the enemy exists (rooms 20F0, 4070, 6160, 7000 test it);
// 7 = flags (obj+0x10e).
int enemy_member_get(int slot, int member, int *value)
{
    if (slot < 0 || slot >= MAX_ENEMIES)
        return 0;
    if (member == 0) {
        *value = enemies[slot].active;
        return 1;
    }
    if (member != 7 || !enemies[slot].active)
        return 0;
    *value = enemies[slot].flags;
    return 1;
}

void enemy_member_set(int slot, int member, int value)
{
    if (slot >= 0 && slot < MAX_ENEMIES && enemies[slot].active && member == 7)
        enemies[slot].flags = value;
}

int enemy_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        n += enemies[i].active;
    return n;
}

// Would moving enemy i to (x, z) push it deeper into another enemy?
static int crowded(int i, int32_t x, int32_t z)
{
    const Enemy *e = &enemies[i];
    for (int j = 0; j < MAX_ENEMIES; j++) {
        const Enemy *o = &enemies[j];
        if (j == i || !o->active)
            continue;
        int64_t ox = o->x - x, oz = o->z - z, cx = o->x - e->x, cz = o->z - e->z;
        int64_t now = ox * ox + oz * oz, before = cx * cx + cz * cz;
        if (now < (int64_t)(4 * ENEMY_RADIUS * ENEMY_RADIUS) && now < before)
            return 1;
    }
    return 0;
}

// Would the player's step from (from_x, from_z) to (x, z) push him deeper
// into an enemy? Same rule as for walls: moving away is always allowed.
int enemy_blocks_player(int32_t from_x, int32_t from_z, int32_t x, int32_t z)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!e->active)
            continue;
        int64_t nx = e->x - x, nz = e->z - z, ox = e->x - from_x, oz = e->z - from_z;
        int64_t now = nx * nx + nz * nz, before = ox * ox + oz * oz;
        if (now < (int64_t)CONTACT_DISTANCE * CONTACT_DISTANCE && now < before)
            return 1;
    }
    return 0;
}

static int32_t frame_speed(const Model *m, const ModelAnim *a, int key)
{
    return model_frame(m, ANIM_FRAME(m->anim_pool[a->first + key]))->speed[0];
}

// DS angle (32768 = 360 degrees) from the enemy to (x, z); facing = (cos, -sin)
static int angle_to(const Enemy *e, int32_t x, int32_t z)
{
    return (int)(atan2f((float)-(z - e->z), (float)(x - e->x)) * 32768.0f / (2.0f * 3.14159265f)) & 32767;
}

// Turns by at most rate RE2 units (4096 = 360 degrees) toward (x, z), away
// from it if rate < 0 (0x80014ef8).
static void turn_toward(Enemy *e, int32_t x, int32_t z, int rate)
{
    int want = angle_to(e, x, z);
    if (rate < 0) {
        rate = -rate;
        want += 16384;
    }
    int diff = ((want - e->dir + 16384) & 32767) - 16384;
    int step = rate * 8;
    if (diff > step)
        e->dir += step;
    else if (diff < -step)
        e->dir -= step;
    else
        e->dir = want;
    e->dir &= 32767;
}

// Is (x, z) within +-half RE2 units of the facing? (0x80014fb4)
static int in_cone(const Enemy *e, int32_t x, int32_t z, int half)
{
    int diff = ((angle_to(e, x, z) - e->dir + 16384) & 32767) - 16384;
    return abs(diff) < half * 8;
}

// Next keyframe. *forward: root motion along the facing (speed x is the
// distance walked since the first keyframe; gaits 4-7 step back a little in
// places, so it may be negative). Returns 1 on the last keyframe.
static int anim_step(Enemy *e, int32_t *forward)
{
    const Model *m = &types[e->type].model;
    const ModelAnim *a = &m->anims[e->anim];
    if (!a->count)
        return 1;
    int prev = e->key;
    e->key = (e->key + 1) % a->count;
    if (forward)
        *forward = frame_speed(m, a, e->key) - (e->key == 0 ? 0 : frame_speed(m, a, prev));
    return e->key == a->count - 1;
}

static void walk(Enemy *e, int i, int32_t step_len)
{
    int32_t start_x = e->x, start_z = e->z;
    int32_t mx = (step_len * cosLerp(e->dir)) >> 12;
    int32_t mz = -((step_len * sinLerp(e->dir)) >> 12);
    if (!game_blocked(e->x, e->z, e->x + mx, e->z, e->y) && !crowded(i, e->x + mx, e->z))
        e->x += mx;
    if (!game_blocked(e->x, e->z, e->x, e->z + mz, e->y) && !crowded(i, e->x, e->z + mz))
        e->z += mz;
    int32_t moved = (int32_t)sqrtf((float)(e->x - start_x) * (e->x - start_x) + (float)(e->z - start_z) * (e->z - start_z));
    if (moved > 200)
        enemy_jumps++;
    if (moved > enemy_jump_max)
        enemy_jump_max = moved;
}

// noise: what Leon does (PS1 0x800c77f2): NOISE_WALK, NOISE_RUN
static void lurk(Enemy *e, int i, int32_t px, int32_t pz, uint32_t d, int noise)
{
    // decision (0x801011fc); the PS1 first turns round to a noise behind
    // (state 12), here the zombie starts chasing at once
    int front90 = in_cone(e, px, pz, 0x400), front70 = in_cone(e, px, pz, 800);
    if ((d < 5000 && front90 && e->sees) ||
        (d < 3000 && !front70 && (noise & 0x15) && e->sees && !(rnd() & 1)) ||
        (d < 2000 && !front90 && e->sees && (noise & 0x17) && (rnd() & 1))) {
        enter_state(e, ST_CHASE);
        return;
    }
    // movement (0x8010135c)
    int32_t fwd;
    if (e->sub == 2) { // shuffle a little toward Leon
        e->sub = 3;
        set_anim(e, SET_GAIT, e->gait, 0);
        e->timer = rnd() + 150;
    }
    if (e->sub == 3) {
        turn_toward(e, px, pz, 16);
        anim_step(e, &fwd);
        walk(e, i, fwd);
        if (e->timer-- == 1)
            enter_state(e, ST_LURK); // stand again
        if (d < 3000 || (d < 7500 && e->sees))
            enter_state(e, ST_CHASE);
        return;
    }
    anim_step(e, NULL); // standing
    if (e->timer2-- == 0)
        e->timer2 = rnd() + 300; // PS1: moans (sound 11) half the time
    if (e->timer2 == 0 && !(rnd() & 1))
        e->sub = 2;
    if (e->timer == 0) {
        if (d < 7500 && e->sees)
            enter_state(e, ST_CHASE);
    } else {
        e->timer--;
    }
}

static void chase(Enemy *e, int i, int32_t px, int32_t pz, uint32_t d)
{
    // movement (0x801019d0)
    if (e->timer-- == 0) {
        e->wander_idx = (e->wander_idx + 1) & 31;
        e->timer = (wander[e->wander_idx] & 0x7FFF) + (rnd() & 0x1F);
        e->dir = (e->dir + ((rnd() & 1) ? 16 : -16) * 8) & 32767;
    }
    int rate = 8;
    if (d > 5000 && (wander[e->wander_idx] & 0x8000))
        rate = -8;
    turn_toward(e, px, pz, rate);
    if (e->turn_timer-- == 0) {
        e->turn_timer = (rnd() & 0xF) + 30;
        turn_toward(e, px, pz, 16); // PS1: may moan (sound 10 / 11)
    }
    if (d < 3000)
        turn_toward(e, px, pz, 16);

    // touching Leon: the PS1 grabs him (state 3, not done yet); stand
    // instead of walking on the spot
    if (d < CONTACT_DISTANCE) {
        if (!e->standing) {
            set_anim(e, SET_MAIN, A_STAND, 0);
            e->standing = 1;
        }
        anim_step(e, NULL);
        return;
    }
    if (e->standing) {
        set_anim(e, SET_GAIT, e->gait, 0);
        e->standing = 0;
    }
    int32_t fwd;
    anim_step(e, &fwd);
    walk(e, i, fwd);
}

static void lying(Enemy *e)
{
    switch (e->sub) {
    case 1: // down until the room script clears the hold flag
        if (!(e->flags & EF_HOLD)) {
            set_anim(e, SET_MAIN, e->lie_alt ? A_GET_UP + 1 : A_GET_UP, rnd() & 0xF);
            e->sub = 3; // PS1: moans (sound 10, 11 or 12)
        }
        break;
    case 3: // getting up
        if (anim_step(e, NULL))
            enter_state(e, ST_CHASE);
        break;
    }
}

void enemy_tick(int32_t px, int32_t py, int32_t pz, int noise)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active || !types[e->type].model.anims[e->anim].count)
            continue;
        float fdx = (float)(px - e->x), fdz = (float)(pz - e->z);
        uint32_t d = (uint32_t)sqrtf(fdx * fdx + fdz * fdz);
        if (e->sight_timer-- == 0) {
            e->sight_timer = 7;
            e->sees = !game_sight_blocked(e->x, e->y + EYE_ENEMY, e->z, px, py + EYE_PLAYER, pz);
        }
        switch (e->state) {
        case ST_LURK: lurk(e, i, px, pz, d, noise); break;
        case ST_CHASE: chase(e, i, px, pz, d); break;
        case ST_LYING: lying(e); break;
        default: anim_step(e, NULL); break;
        }
    }
}

static void draw_bone(const EnemyType *t, int bone, int frame)
{
    const Model *m = &t->model;
    const ModelBone *b = &m->bones[bone];
    const ModelFrame *f = model_frame(m, frame);
    const int16_t *rot = model_frame_rot(m, frame) + bone * 3;
    glPushMatrix();
    if (b->parent < 0)
        glTranslatef32(f->offset[0], f->offset[1], f->offset[2]);
    else
        glTranslatef32(b->pos[0], b->pos[1], b->pos[2]);
    glRotateXi(rot[0]);
    glRotateYi(rot[1]);
    glRotateZi(rot[2]);
    for (int p = 0; p < t->pages; p++) {
        uint32_t off = m->lists[bone * t->pages + p];
        if (off) {
            glBindTexture(0, t->tex[p]);
            glCallList((const u32 *)((const uint8_t *)m->data + off));
        }
    }
    for (int c = t->first_child[bone]; c >= 0; c = t->next_sibling[c])
        draw_bone(t, c, frame);
    glPopMatrix(1);
}

// Is the point roughly inside the current view? (camera from main.c)
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t margin);
extern int cull_models;  // main.c: POLY_CULL_* that drops faces turned away

void enemy_draw(int reserve_vertices, int reserve_polygons)
{
    // nearest first: simple selection on the distance to the camera
    int order[MAX_ENEMIES], n = 0;
    int32_t dist[MAX_ENEMIES];
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!e->active || !types[e->type].drawable || !types[e->type].model.anims[e->anim].count)
            continue;
        int32_t d = camera_sees(e->x, e->y - 900, e->z, 1200);
        if (d < 0) {
            enemies[i].drawn = 0;
            enemies[i].was_visible = 0;
            continue; // off screen
        }
        if (e->drawn)
            d -= KEEP_BONUS;
        int j = n++;
        while (j > 0 && dist[j - 1] > d) {
            order[j] = order[j - 1];
            dist[j] = dist[j - 1];
            j--;
        }
        order[j] = i;
        dist[j] = d;
    }
    for (int k = 0; k < n; k++) {
        Enemy *e = &enemies[order[k]];
        const EnemyType *t = &types[e->type];
        e->drawn = 0;
        // never drawn yet: all faces, or about 60 % with culling
        int est_v = e->cost_v, est_p = e->cost_p;
        if (!est_v) {
            est_v = cull_models == POLY_CULL_NONE ? t->vertices : t->vertices * 3 / 5;
            est_p = cull_models == POLY_CULL_NONE ? t->polygons : t->polygons * 3 / 5;
        }
        while (GFX_BUSY)
            ; // the counters are exact once the geometry engine is idle
        int v0 = GFX_VERTEX_RAM_USAGE, p0 = GFX_POLYGON_RAM_USAGE;
        if (v0 + est_v + reserve_vertices > VERTEX_LIMIT || p0 + est_p + reserve_polygons > POLYGON_LIMIT)
            continue; // a cheaper one further back may still fit
        const ModelAnim *a = &t->model.anims[e->anim];
        int frame = ANIM_FRAME(t->model.anim_pool[a->first + e->key]);
        glPolyFmt(POLY_ALPHA(31) | cull_models | POLY_ID(3 + order[k]));
        glPushMatrix();
        glTranslatef32(e->x, e->y, e->z);
        glRotateYi(e->dir);
        for (int b = 0; b < t->model.header->num_bones; b++)
            if (t->model.bones[b].parent < 0)
                draw_bone(t, b, frame);
        glPopMatrix(1);
        while (GFX_BUSY)
            ;
        e->cost_v = GFX_VERTEX_RAM_USAGE - v0;
        e->cost_p = GFX_POLYGON_RAM_USAGE - p0;
        e->drawn = 1;
    }
    for (int k = 0; k < n; k++) {
        Enemy *e = &enemies[order[k]];
        if (e->was_visible && e->was_drawn != e->drawn)
            enemy_toggles++;
        e->was_visible = 1;
        e->was_drawn = e->drawn;
    }
}

#ifndef ARM9
// host tests: positions and step per tick
void enemy_debug_print(int tick)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (e->active)
            printf("t%d e%d st %d/%d anim %d key %d pos %d %d dir %d verts %d\n", tick, i, e->state, e->sub, e->anim, e->key,
                   (int)e->x, (int)e->z, e->dir, types[e->type].vertices);
    }
}
#endif
