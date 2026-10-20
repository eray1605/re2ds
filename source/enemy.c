// re2ds - enemies (see enemy.h).
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "enemy.h"
#include "plc.h"

#define MAX_TYPES 4             // models loaded at the same time (texture memory)
#define MAX_PAGES 4
#define ENEMY_RADIUS 400
#define CONTACT_DISTANCE 800    // ENEMY_RADIUS + the player's radius (main.c)
#define MAX_SETS 4

// Zombie behaviour after the PS1 code (zombie overlay of CDEMD0.EMS, linked at
// 0x80100000; docs/enemy-zombie.md). States as in obj+5; only the ones
// that need no attacks yet. One game tick here is one PS1 frame.
#define ST_LURK 0               // stands, shuffles now and then (0x801011fc / 0x8010135c)
#define ST_CHASE 1              // shuffles toward Leon (0x8010167c / 0x801019d0)
#define ST_LUNGE 2              // arms up, faster at Leon (0x80101f0c / 0x80102218)
#define ST_GRAB 3               // grabs and bites (0x8010259c / 0x801025a4)
#define ST_LYING 7              // lies until EF_HOLD goes, then gets up (0x801036b4)
#define ST_EAT 8                // eats (variant 8, or Leon's body)
#define ST_DOWN 5               // knocked down by shots, gets up again
#define ST_DYING 20             // falls dead (death routine 0x80108470), then lies
#define ST_DEAD 21

// Enemy.flags = sce_em_set bytes 4..5 (obj+0x10e): spawn variant | status << 8.
// Room scripts read and write it as member 7.
#define EF_VARIANT 0x003F       // 0/3 lurk, 1/6 chase, 2/4 lying, 5/7 lying as dead, 8 eat
#define EF_HOLD 0x4000          // lying: stay down until the room script clears it
#define EF_OFF 0x8000           // not processed nor drawn until the room script clears it
                                // (often parked outside the room, e.g. 1020, 1060, 11D0)

// Animations: set 0 holds the gaits, set 1 everything else (emXX_sets.bin)
#define SET_GAIT 0
#define SET_MAIN 1
#define A_STAND 0x00
#define A_GET_UP 0x08           // 0x09 from the other lying pose
#define A_EAT 0x12
#define A_LIE 0x17              // 0x16 = the other lying pose
#define A_GRAB 0x0B             // grab; + 1 bite (loops), + 2 pushed away
#define SET_PLAYER 2            // set 2 is for Leon's skeleton (the PS1 plays it on him)
#define P_GRAB_FRONT 0          // grabbed from the front; + 1 struggle, + 2 break free
#define P_GRAB_BACK 3           // from behind
#define P_DIE_FRONT 0x0D
#define P_DIE_BACK 0x0F

#define A_FALL 0x01             // + 0 / 1: falls forward / backward (death, knockdown)

// health (0x8010c494): (rand >> (rand & 3)) & 15 picks one, +15 early in the game
static const uint8_t hp_table[16] = { 80, 94, 128, 75, 60, 95, 58, 75, 50, 83, 79, 66, 80, 65, 82, 65 };
// stagger lost per hit, by weapon id (0x8010ca57); at 0 twice it falls over
static const uint8_t stagger_hit[20] = { 0, 9, 15, 15, 35, 0, 0, 0, 0, 0, 0, 0, 20, 0, 0, 0, 0, 0, 0, 0 };

#define GRAB_DISTANCE 1200
#define BITE_KEY 16             // keyframe of the bite (table 0x80100014)
#define BITE_DAMAGE 20          // before the PS1's x1.5 (FUN_8003cf38): 30 HP
#define STRUGGLE 148            // -2 per frame, -5 more per frame with a key pressed

// Enemy sounds (bank 3, sound/enemNN.bin), + 16 for the room's second voice
#define SND_BITE 3
#define SND_MOAN 10             // 10..13
#define MOAN_PAUSE 150          // frames between moans (obj+0x239)

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
// sounds and Leon while he is held (main.c)
void game_enemy_sound(int sound, int32_t x, int32_t z);
void game_player_voice(int sound);         // CORE00: 0 / 2 grabbed, 3 dies
int game_player_damage(int damage);       // 0 hurt, 1 down to 0 HP once (breaks free), 2 dies
void game_player_grabbed(int on);
void game_player_pose(const Model *set, int anim, int key, int32_t x, int32_t z, int16_t dir);
void game_player_died(void);              // death animation over
void game_player_hurt(int kind);          // hit while not held: 0 light, 1 front, 2 from behind
void game_player_killed(void);            // killed while not held
// blood sprite (effect.h kinds), sprayed toward dir (DS units)
void game_blood(int kind, int32_t x, int32_t y, int32_t z, int16_t dir, int speed, int32_t floor_y);

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

#define POSE_BONES 32
#define POSE_BLEND_TICKS 7

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
    uint8_t voice, voice_ofs;   // voice kind (sce_em_set byte 7); + 16 for the second kind
    uint8_t moan_pause;
    int16_t hp;                 // obj+0x156
    int8_t stagger;             // obj+0x223
    uint8_t stagger_out;        // reached 0 once (obj+0x222)
    uint8_t flinch;             // frames of the hit recoil left
    int16_t push;               // shotgun impulse along push_dir, -50 per tick (obj+0x144)
    int16_t push_dir;           // DS units
    uint8_t bleed;              // ticks of blood after a heavy hit (obj+0x15a)
    uint8_t fall_back;          // death / knockdown: falls backward
    uint8_t routine;            // obj+4: 1 = own AI, 8 = room script's zombie modes
    uint8_t mode;               // obj+5 in routine 8 (state in routine 1)
    int16_t dest_x, dest_z;     // obj+0x1c4 / 0x1c6 (members 0x14 / 0x15)
    int8_t target;              // obj+0x1da (member 0x1a): object index, 2 + slot
    uint8_t has_goal;
    int16_t goal_x, goal_z;     // where it heads for Leon (obj+0x1c4 / 0x1c6 in routine 1)
    Plc plc;                    // routine 4: the room script moves it (plc.c)
    uint8_t victim;             // a person a zombie got (routine 8 mode 2): falls, then lies
    const Model *anim_set;      // e->anim is in this set (the room's), NULL = its own model
    // shown pose: eases into a new animation over POSE_BLEND_TICKS like Leon's
    // (FUN_80028a4c), so a person switching gestures does not snap
    int16_t pose_rot[POSE_BONES][3];
    int16_t pose_ofs[3];
    const Model *pose_set;
    int16_t pose_anim;
    uint8_t pose_blend, pose_valid;
    uint8_t no_blend;           // plc flag 0x40: the next animation starts without easing
    // dogs (dog_tick)
    int32_t floor_y;            // the floor it stands on (y changes in a jump)
    int16_t speed, vspeed;      // forward / vertical speed per tick (obj+0x144 / 0x146)
    int16_t misses;             // ticks running without Leon in front, x10 (obj+0x21a)
    int16_t turned;             // turned so far in a U-turn or a bounce (obj+0x158), DS units
    int16_t circ_x, circ_z;     // offset from Leon it circles toward (obj+0x228 / 0x22a)
    int16_t turn_rate;          // turning while hunting (obj+0x224), RE2 units
    int16_t aggr;               // aggression (obj+0x16a)
    uint8_t cooldown;           // ticks before it turns to Leon again (obj+0x21d)
    uint8_t airborne;           // in a jump (obj+0x219)
} Enemy;

// The grab: one zombie at a time (PS1 flag 0x80 in 0x800c79c7). Zombie and
// Leon stand at an anchor plus their animation's offset (speed x/y/z), turned
// to the zombie's facing (FUN_80015534 / FUN_80015658), so the two animations
// fit together.
static struct {
    int8_t enemy;               // -1 = none
    uint8_t back;               // grabbed from behind
    uint8_t step;               // Leon: 1 grab, 3 struggle, 4/5 break free, 6/7 dies
    int32_t ax, az;             // anchor
    int16_t dir;                // the zombie's facing when it grabbed
    int anim, key;              // Leon's animation (zombie model, set 2)
    int16_t struggle;
} grab = { .enemy = -1 };
static uint8_t voice_first, voice_second; // PS1 0x800d0818 / 0x800d0819

static uint8_t dog_alert;       // a dog has jumped at Leon: the others hunt too (0x800c77f0 & 0x20)
static int8_t crow_lock = -1;   // the crow attacking now (0x800c77f0 & 1)
static int8_t claw_lock = -1;   // the licker clawing now (0x8010752c)
static EnemyType types[MAX_TYPES];
static Enemy enemies[MAX_ENEMIES];
static const Model *room_sets[MAX_ENEMIES]; // the room's animations per slot (RBJ masks)

// People (types 0x40 and up: Kendo 0x48, ...) do not fight; the room scripts
// move them, otherwise they stand.
#define FIRST_PERSON_TYPE 0x40
static int is_person(const Enemy *e) { return types[e->type].type >= FIRST_PERSON_TYPE; }
static int is_dog(const Enemy *e);
static void dog_spawn(Enemy *e);
static int is_crow(const Enemy *e);
static void crow_spawn(Enemy *e);
static int is_licker(const Enemy *e);
static void licker_spawn(Enemy *e);
static int is_spider(const Enemy *e);
static void spider_spawn(Enemy *e);
static int is_tyrant(const Enemy *e);
static void tyrant_spawn(Enemy *e);

// the set e->anim / e->key refer to
static const Model *anim_model(const Enemy *e) { return e->anim_set ? e->anim_set : &types[e->type].model; }

void enemy_set_room_set(int slot, const Model *set)
{
    if (slot >= 0 && slot < MAX_ENEMIES)
        room_sets[slot] = set;
}

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
    grab.enemy = -1;
    voice_first = voice_second = 0;
    dog_alert = 0;
    crow_lock = -1;
    claw_lock = -1;
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
        e->timer2 = (rnd() >> 3) + 100;
        break;
    case ST_LUNGE:
        set_anim(e, SET_GAIT, e->gait + 4, rnd() & 0x1F); // the arms-up gaits
        e->timer = (rnd() & 0x3F) + 180;
        break;
    case ST_LYING:
        set_anim(e, SET_MAIN, e->lie_alt ? A_LIE - 1 : A_LIE, 0);
        break;
    case ST_EAT:
        set_anim(e, SET_MAIN, A_EAT, 0);
        break;
    }
}

void enemy_spawn(int slot, int type, int flags, int voice, int32_t x, int32_t y, int32_t z, int16_t dir_re2)
{
    enemy_spawn_calls++;
    if (slot == 0xFF)
        slot = PARTNER_SLOT;
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
    e->voice = voice;
    if (voice && voice != voice_first) { // PS1: the room's first and a second voice kind
        if (!voice_first)
            voice_first = voice;
        else
            voice_second = voice;
    }
    e->gait = gaits[(rnd() >> (rnd() & 3)) & 7];
    e->hp = hp_table[(rnd() >> (rnd() & 3)) & 15] + 15;
    e->stagger = (rnd() & 0xF) + 0x10;
    e->sight_timer = slot & 7;
    e->routine = 1;

    if (is_dog(e)) {
        dog_spawn(e);
        return;
    }
    if (is_crow(e)) {
        crow_spawn(e);
        return;
    }
    if (is_licker(e)) {
        licker_spawn(e);
        return;
    }
    if (is_spider(e)) {
        spider_spawn(e);
        return;
    }
    if (is_tyrant(e)) {
        tyrant_spawn(e);
        return;
    }
    // start state by spawn variant (zombie init 0x80100674)
    int state = ST_LURK;
    switch (flags & EF_VARIANT) {
    case 2: e->lie_alt = 1; state = ST_LYING; break;
    case 4: state = ST_LYING; break;
    case 5: e->lie_alt = 1; e->flags |= EF_HOLD; state = ST_LYING; break; // lies as if dead
    case 7: e->flags |= EF_HOLD; state = ST_LYING; break;
    case 1: state = ST_LUNGE; break;
    case 6: state = ST_CHASE; break;
    case 8: state = ST_EAT; break;
    }
    enter_state(e, state);
    if (is_person(e))
        set_anim(e, SET_MAIN, 2, 0);
}

// The room's enemy sound set (FUN_8004f1d0): the first pair of voice kinds in
// pairs (u8 count, then (first, second) per set) that matches the room's;
// enemies of the pair's second kind use sound + 16. -1 if none.
int enemy_sound_set(const uint8_t *pairs)
{
    if (!pairs || !voice_first)
        return -1;
    for (int i = 0; i < pairs[0]; i++) {
        int a = pairs[1 + 2 * i], b = pairs[2 + 2 * i];
        int fits = a == voice_first && (!voice_second || voice_second == b);
        int swapped = !fits && b == voice_first && (!voice_second || voice_second == a);
        if (!fits && !swapped)
            continue;
        for (int k = 0; k < MAX_ENEMIES; k++) {
            Enemy *e = &enemies[k];
            e->voice_ofs = (e->voice == voice_first && swapped) ||
                           (e->voice == voice_second && voice_second && !swapped) ? 16 : 0;
        }
        return i;
    }
    return -1;
}

static void sound_at(const Enemy *e, int sound)
{
    game_enemy_sound(sound + e->voice_ofs, e->x, e->z);
}

static void moan(Enemy *e, int sound)
{
    if (e->moan_pause)
        return;
    sound_at(e, sound);
    e->moan_pause = MOAN_PAUSE;
}

// Members the room scripts read (FUN_8005202c lists all): 0 = object flags,
// bit 0 set while the enemy exists (rooms 20F0, 4070, 6160, 7000 test it);
// 7 = flags (obj+0x10e).
// 2 / 3 = routine and state (obj+4 / 5), 0x0B-0x0D position, 0x0F facing,
// 0x14 / 0x15 destination, 0x1A target object (routine 8).
int enemy_member_get(int slot, int member, int *value)
{
    if (slot < 0 || slot >= MAX_ENEMIES)
        return 0;
    const Enemy *e = &enemies[slot];
    if (member == 0) {
        *value = e->active;
        return 1;
    }
    if (!e->active)
        return 0;
    switch (member) {
    case 2: *value = e->routine; return 1;
    case 3: *value = e->routine == 1 ? e->state : e->mode; return 1;
    case 7: *value = e->flags; return 1;
    case 0x0B: *value = e->x; return 1;
    case 0x0C: *value = e->y; return 1;
    case 0x0D: *value = e->z; return 1;
    case 0x0F: *value = e->dir / 8; return 1;
    }
    return 0;
}

static void start_script_mode(Enemy *e);

void enemy_member_set(int slot, int member, int value)
{
    if (slot < 0 || slot >= MAX_ENEMIES || !enemies[slot].active)
        return;
    Enemy *e = &enemies[slot];
    switch (member) {
    case 2: e->routine = value; break;
    case 3: // the state / mode starts once both are written (scripts set 2 first)
        if (e->routine == 8) {
            e->mode = value;
            start_script_mode(e);
        } else if (e->routine == 1 && value != e->state) {
            enter_state(e, value);
        }
        break;
    case 7: e->flags = value; break;
    case 0x0B: e->x = (int16_t)value; break;
    case 0x0C: e->y = (int16_t)value; break;
    case 0x0D: e->z = (int16_t)value; break;
    case 0x0F: e->dir = ((int16_t)value * 8) & 32767; break;
    case 0x14: e->dest_x = value; break;
    case 0x15: e->dest_z = value; break;
    case 0x1A: e->target = value; break;
    }
}

int enemy_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        n += enemies[i].active;
    return n;
}

// exists and takes part (EF_OFF: parked by the room script)
static int present(const Enemy *e) { return e->active && !(e->flags & EF_OFF); }

// Would moving enemy i to (x, z) push it deeper into another enemy?
static int crowded(int i, int32_t x, int32_t z)
{
    const Enemy *e = &enemies[i];
    for (int j = 0; j < MAX_ENEMIES; j++) {
        const Enemy *o = &enemies[j];
        if (j == i || !present(o) || o->state >= ST_DYING)
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
        if (!present(e) || e->state >= ST_DYING || e->state == ST_DOWN)
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
        turn_toward(e, e->has_goal ? e->goal_x : px, e->has_goal ? e->goal_z : pz, 16);
        anim_step(e, &fwd);
        walk(e, i, fwd);
        if (e->timer-- == 1)
            enter_state(e, ST_LURK); // stand again
        if (d < 3000 || (d < 7500 && e->sees))
            enter_state(e, ST_CHASE);
        return;
    }
    anim_step(e, NULL); // standing
    if (e->timer2-- == 0) {
        e->timer2 = rnd() + 300;
        if (rnd() & 1)
            moan(e, SND_MOAN + 1);
    }
    if (e->timer2 == 0 && !(rnd() & 1))
        e->sub = 2;
    if (e->timer == 0) {
        if (d < 7500 && e->sees)
            enter_state(e, ST_CHASE);
    } else {
        e->timer--;
    }
}

// Grab Leon: he must be within +-45 degrees in front (two +-22.5 degree cones
// for the arms, FUN_800150f8), closer than 1200, and nobody else holds him.
static int can_grab(const Enemy *e, const EnemyTarget *p, uint32_t d)
{
    return d < GRAB_DISTANCE && grab.enemy < 0 && p->can_grab && in_cone(e, p->x, p->z, 0x200);
}

// Types without their own behaviour yet (spiders, the alligator, G, plants,
// ...) run the zombie's, but their models have no grab (set 2 is the
// zombie's animations for Leon): within reach they strike instead, 10 HP
// (x1.5) every 40 ticks.
#define TYPE_ZOMBIE_LAST 0x1F
static int is_zombie(const Enemy *e) { return types[e->type].type <= TYPE_ZOMBIE_LAST; }
static int strike(Enemy *e, const EnemyTarget *p, uint32_t d)
{
    if (is_zombie(e) || d >= GRAB_DISTANCE || !p->can_grab || !in_cone(e, p->x, p->z, 0x200))
        return 0;
    if (e->cooldown) {
        e->cooldown--;
        return 0;
    }
    e->cooldown = 40;
    sound_at(e, SND_BITE);
    game_blood(0, p->x, p->y - 1500, p->z, e->dir, 20, p->y);
    if (game_player_damage(10) == 2)
        game_player_killed();
    else
        game_player_hurt(((p->dir - e->dir + 8192) & 32767) < 16384 ? 2 : 1);
    return 1;
}

// World position of an animation offset (speed x/y/z) of the grab, turned
// like the PS1's RotMatrixY(dir) (x' = c x + s z, z' = -s x + c z).
static void turned_offset(const Model *m, int anim, int key, int16_t dir, int32_t *x, int32_t *z)
{
    const ModelAnim *a = &m->anims[anim];
    const ModelFrame *f = model_frame(m, ANIM_FRAME(m->anim_pool[a->first + key]));
    int32_t c = cosLerp(dir), sn = sinLerp(dir);
    *x = (f->speed[0] * c + f->speed[2] * sn) >> 12;
    *z = (-f->speed[0] * sn + f->speed[2] * c) >> 12;
}

static void grab_place(const Model *m, int anim, int key, int32_t *x, int32_t *z)
{
    turned_offset(m, anim, key, grab.dir, x, z);
    *x += grab.ax;
    *z += grab.az;
}

static void start_grab(Enemy *e, int i, const EnemyTarget *p)
{
    const Model *m = &types[e->type].model;
    e->state = ST_GRAB;
    e->sub = 1;
    e->standing = 0;
    e->dir = angle_to(e, p->x, p->z); // turns to Leon at once
    set_anim(e, SET_MAIN, A_GRAB, 0);
    grab.enemy = i;
    grab.dir = e->dir;
    // from behind if Leon looks the same way as the zombie (FUN_800152b0)
    grab.back = ((p->dir - e->dir + 8192) & 32767) < 16384;
    // anchor: the zombie's position minus its own animation offset
    grab.ax = grab.az = 0;
    int32_t ox, oz;
    grab_place(m, e->anim, e->key, &ox, &oz);
    grab.ax = e->x - ox;
    grab.az = e->z - oz;
    grab.step = 1;
    grab.anim = types[e->type].set_start[SET_PLAYER] + (grab.back ? P_GRAB_BACK : P_GRAB_FRONT);
    grab.key = 0;
    game_player_grabbed(1);
    game_player_voice((rnd() & 1) ? 2 : 0);
    moan(e, (rnd() & 1) ? SND_MOAN : SND_MOAN + 1);
}

// Leon's side of the grab (FUN_8010a8f8 front / back, FUN_8010b3a4 dying)
static void grab_player(const Enemy *e)
{
    const EnemyType *t = &types[e->type];
    const Model *m = &t->model;
    int base = t->set_start[SET_PLAYER];
    int count = m->anims[grab.anim].count;
    switch (grab.step) {
    case 1: // grabbed
        if (++grab.key >= count) {
            grab.anim = base + (grab.back ? P_GRAB_BACK : P_GRAB_FRONT) + 1;
            grab.key = 0;
            grab.step = 3;
        }
        break;
    case 3: // struggles
        grab.key = (grab.key + 1) % count;
        break;
    case 4: // breaks free
        grab.anim = base + (grab.back ? P_GRAB_BACK : P_GRAB_FRONT) + 2;
        grab.key = 0;
        grab.step = 5;
        break;
    case 5:
        if (++grab.key >= count) {
            grab.enemy = -1;
            game_player_grabbed(0);
            return;
        }
        break;
    case 6: // dies
        grab.anim = base + (grab.back ? P_DIE_BACK : P_DIE_FRONT);
        grab.key = 0;
        grab.step = 7;
        game_player_voice(3);
        break;
    case 7:
        if (grab.key + 1 >= count) {
            game_player_died();
            return;
        }
        grab.key++;
        break;
    }
    int32_t x, z;
    grab_place(m, grab.anim, grab.key, &x, &z);
    // front: Leon faces the zombie; from behind he looks the same way
    int16_t pdir = grab.back ? grab.dir : (grab.dir + 16384) & 32767;
    game_player_pose(m, grab.anim, grab.key, x, z, pdir);

    // blood at his neck (part 8): a splash per bite, spurts while he dies
    // (FUN_8001b8fc 0x17d0 / 0xbd0 in FUN_8010a8f8 and FUN_8010b3a4)
    if (grab.step == 3 && grab.key == 15) {
        int16_t dir = pdir + (rnd() * 2 + (grab.back ? 0x800 : 0) - 0x100) * 8;
        game_blood(0, x, e->y - 2200, z, dir, 30, e->y);
        game_blood(2, x, e->y - 2200, z, dir + 0x800, 20, e->y);
    } else if (grab.step == 7 && grab.key >= 41 && grab.key <= 98 && !(grab.key & 1)) {
        int32_t y = e->y - 2200 + (grab.key - 41) * 30;
        game_blood(1, x, y, z, rnd() << 7, 15 + (rnd() & 7), e->y);
    } else if (grab.step == 7 && grab.key >= 100 && grab.key < 105) {
        game_blood(0, x, e->y - 300, z, rnd() << 7, 8, e->y);
    }
}

// The zombie's side (0x801025a4)
static void grab_zombie(Enemy *e, int i, int pressed)
{
    const Model *m = &types[e->type].model;
    int32_t x, z;
    switch (e->sub) {
    case 1: // grabs
        if (anim_step(e, NULL)) {
            set_anim(e, SET_MAIN, A_GRAB + 1, 0);
            grab.struggle = STRUGGLE;
            e->sub = 3;
        }
        break;
    case 3: // bites
        anim_step(e, NULL);
        if (grab.step == 3) {
            grab.struggle -= 2 + (pressed ? 5 : 0);
            if (grab.struggle < 0) {
                e->sub = 4;
                grab.step = 4;
                break;
            }
        }
        if (e->key == BITE_KEY) {
            sound_at(e, SND_BITE);
            int r = game_player_damage(BITE_DAMAGE);
            if (r == 1) {         // down to 0 HP the first time: he tears free
                e->sub = 4;
                grab.step = 4;
            } else if (r == 2) {  // dead: keeps eating
                e->sub = 9;
                grab.step = 6;
            }
        }
        break;
    case 4: // pushed away
        set_anim(e, SET_MAIN, A_GRAB + 2, 0);
        e->sub = 5;
        break;
    case 5: { // staggers back; from key 7 on it has let go and collides
        int32_t x0, z0;
        turned_offset(m, e->anim, e->key, e->dir, &x0, &z0);
        int done = anim_step(e, NULL);
        if (e->key < 7) {
            grab_place(m, e->anim, e->key, &x, &z);
            e->x = x;
            e->z = z;
        } else {
            turned_offset(m, e->anim, e->key, e->dir, &x, &z);
            int32_t dx = x - x0, dz = z - z0;
            if (!game_blocked(e->x, e->z, e->x + dx, e->z, e->y))
                e->x += dx;
            if (!game_blocked(e->x, e->z, e->x, e->z + dz, e->y))
                e->z += dz;
        }
        if (done) {
            if (grab.enemy == i && grab.step < 4)
                grab.enemy = -1;
            enter_state(e, ST_LURK);
        }
        return;
    }
    case 9: // eats the body
        anim_step(e, NULL);
        break;
    }
    if (e->sub != 5) {
        grab_place(m, e->anim, e->key, &x, &z);
        e->x = x;
        e->z = z;
    }
}

static void chase(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    int32_t px = p->x, pz = p->z;
    int32_t gx = e->has_goal ? e->goal_x : px, gz = e->has_goal ? e->goal_z : pz; // path
    // decision (0x8010167c): grab, or eat Leon's body
    if (strike(e, p, d))
        return;
    if (is_zombie(e) && can_grab(e, p, d)) {
        start_grab(e, i, p);
        return;
    }
    if (p->dead && d < 1000 && in_cone(e, px, pz, 0x200)) {
        enter_state(e, ST_EAT);
        return;
    }
    // movement (0x801019d0)
    if (e->timer-- == 0) {
        e->wander_idx = (e->wander_idx + 1) & 31;
        e->timer = (wander[e->wander_idx] & 0x7FFF) + (rnd() & 0x1F);
        e->dir = (e->dir + ((rnd() & 1) ? 16 : -16) * 8) & 32767;
    }
    int rate = 8;
    if (d > 5000 && (wander[e->wander_idx] & 0x8000))
        rate = -8;
    turn_toward(e, gx, gz, rate);
    if (e->turn_timer-- == 0) {
        e->turn_timer = (rnd() & 0xF) + 30;
        turn_toward(e, gx, gz, 16);
        if (!(rnd() & 0x1F))
            moan(e, SND_MOAN);
        else if (!(rnd() & 0x1F))
            moan(e, SND_MOAN + 1);
    }
    if (d < 3000)
        turn_toward(e, gx, gz, 16);

    // touching Leon while another zombie holds him: stand instead of
    // walking on the spot
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

    // now and then, or when Leon is close in front or runs: arms up (state 2)
    if (e->timer2-- == 1) {
        e->timer2 = (rnd() >> 3) + 100;
        if (!(rnd() & 3) && d < 5000)
            goto lunge;
    }
    if ((d < 2000 && in_cone(e, px, pz, 0x200) && grab.enemy < 0) ||
        ((p->noise & NOISE_RUN) && d < 2500 && in_cone(e, px, pz, 0x52C)))
        goto lunge;
    return;
lunge:
    enter_state(e, ST_LUNGE);
    if (rnd() & 1)
        moan(e, SND_MOAN + 1);
    else
        moan(e, SND_MOAN);
}

// arms up, toward Leon (0x80102218); back to shuffling after a while
static void lunge(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (strike(e, p, d))
        return;
    if (is_zombie(e) && can_grab(e, p, d)) {
        start_grab(e, i, p);
        return;
    }
    turn_toward(e, e->has_goal ? e->goal_x : p->x, e->has_goal ? e->goal_z : p->z, 16);
    if (d >= CONTACT_DISTANCE) {
        int32_t fwd;
        anim_step(e, &fwd);
        walk(e, i, fwd);
    } else {
        anim_step(e, NULL);
    }
    if (e->timer-- == 0 || grab.enemy >= 0 ||
        ((d > 5000 || !in_cone(e, p->x, p->z, 0x400)) && e->timer < 120))
        enter_state(e, ST_CHASE);
}

static void lying(Enemy *e)
{
    switch (e->sub) {
    case 1: // down until the room script clears the hold flag
        if (!(e->flags & EF_HOLD)) {
            set_anim(e, SET_MAIN, e->lie_alt ? A_GET_UP + 1 : A_GET_UP, rnd() & 0xF);
            e->sub = 3;
            moan(e, (rnd() & 1) ? SND_MOAN + 2 : (rnd() & 1) ? SND_MOAN + 1 : SND_MOAN);
        }
        break;
    case 3: // getting up
        if (anim_step(e, NULL))
            enter_state(e, ST_CHASE);
        break;
    }
}

static void fallen(Enemy *e, int i);
static void push_tick(Enemy *e, int i);

// Routine 8 (zombie 0x80109c3c, modes from table 0x8010cd3c): the room
// script drives the zombie. 0 stands, 1 shuffles toward the destination
// (turn 0x24, gait from set 0), 2 lunges at the target object and holds it,
// 3 eats the target.
#define A_SCRIPT_LUNGE 0x0A
#define A_SCRIPT_HOLD 0x18
#define A_SCRIPT_EAT 0x15
#define LUNGE_HOLD_KEY 0x11
static void start_script_mode(Enemy *e)
{
    e->sub = 1;
    e->flinch = 0;
    switch (e->mode) {
    case 0: set_anim(e, SET_MAIN, A_STAND, 0); break;
    case 1: set_anim(e, SET_GAIT, e->gait, 0); break;
    case 2: set_anim(e, SET_MAIN, A_SCRIPT_LUNGE, 0); break;
    case 3: set_anim(e, SET_MAIN, A_SCRIPT_EAT, rnd() & 7); break;
    }
}

static void script_mode(Enemy *e, int i)
{
    int32_t step;
    const Enemy *t = e->target >= 2 && e->target - 2 < MAX_ENEMIES ? &enemies[e->target - 2] : NULL;
    switch (e->mode) {
    case 1:
        turn_toward(e, e->dest_x, e->dest_z, 0x24);
        anim_step(e, &step);
        walk(e, i, step);
        break;
    case 2:
        if (t)
            turn_toward(e, t->x, t->z, 0x400);
        anim_step(e, NULL);
        if (e->sub == 1 && e->key == LUNGE_HOLD_KEY) {
            e->sub = 2;
            set_anim(e, SET_MAIN, A_SCRIPT_HOLD, 0);
            // the victim plays the zombie's set 2 (FUN_80015534): here its fall
            Enemy *v = e->target >= 2 && e->target - 2 < MAX_ENEMIES ? &enemies[e->target - 2] : NULL;
            const EnemyType *zt = &types[e->type];
            if (v && v->active && is_person(v) && zt->num_sets > SET_PLAYER) {
                v->plc.active = 0;
                v->victim = 1;
                v->anim_set = &zt->model;
                v->anim = zt->set_start[SET_PLAYER] + P_DIE_FRONT;
                v->key = 0;
            }
        }
        break;
    case 3:
        if (t)
            turn_toward(e, t->x, t->z, 0x10);
        anim_step(e, NULL);
        break;
    default:
        anim_step(e, NULL);
        break;
    }
}

// ---------------------------------------------------------------- dog
// Cerberus (type 0x20; dog overlay of CDEMD0.EMS, linked at 0x80100000,
// states in table 0x80105288; docs/enemy-others.md). Unlike the zombie it
// moves by a speed of its own (obj+0x144, FUN_80014c68), not by the
// animation's root motion. All its animations are in set 0.
#define TYPE_DOG 0x20
#define DOG_IDLE 0              // walks about, sniffs (state 0, 0x80100708 / 0x801004cc)
#define DOG_CIRCLE 1            // runs round Leon at a distance (0x801007a8)
#define DOG_HUNT 2              // runs at him (0x80100aa4, sub states 0x801052ec)
#define DOG_ATTACK 3            // jumps and bites (0x80101170, sub states 0x8010530c)
#define DOG_HURT 9              // thrown back by a hit, gets up (0x801031d8, 0x8010335c, 0x8010347c)
#define D_SNIFF 0x00
#define D_WALK 0x01
#define D_RUN 0x02              // turning: 0x0D left, 0x0C right (0x801054cc)
#define D_RUN_LEFT 0x0D
#define D_RUN_RIGHT 0x0C
#define D_GET_UP 0x07
#define D_THROWN 0x11
#define D_DOWN 0x12
#define D_JUMP 0x14
#define D_LAND 0x15
#define D_BOUNCE 0x16
#define DOG_GRAVITY 40
#define DOG_BITE 20             // x1.5 (FUN_8003cf38): 30 HP
#define SND_DOG_HIT 1
#define SND_DOG_JUMP 3
#define SND_DOG_DIE 7
#define SND_DOG_LAND 10
#define SND_DOG_LAND_HARD 11

// health (0x801051d4) + rand & 3
static const uint8_t dog_hp[16] = { 129, 95, 95, 95, 129, 80, 95, 95, 80, 95, 69, 80, 69, 95, 69, 80 };
// circling: offsets from Leon, x and z picked apart (0x801052cc)
static const int16_t dog_offset[8] = { 1024, 0, -512, 0, -1024, 0, 512, 0 };
// pause before it turns to Leon again after a bite (0x80105244)
static const uint8_t dog_pause[8] = { 7, 31, 15, 15, 15, 15, 7, 31 };
// thrown back along Leon's facing plus one of these (0x801053c0), RE2 units
static const int16_t dog_throw[8] = { 0x80, -0xC0, -0x80, 0xC0, -0x100, 0x100, 0x100, -0x100 };
// damage by weapon (near / middle / far), docs/enemy-others.md; 0 = the
// weapon's own (zombie) values
static const uint16_t dog_damage[20][3] = {
    [1] = { 10, 10, 10 },      // knife
    [2] = { 18, 16, 15 }, [3] = { 18, 16, 15 }, [4] = { 18, 16, 15 },
    [5] = { 200, 200, 200 }, [6] = { 300, 300, 300 },
    [7] = { 59, 40, 30 }, [8] = { 300, 75, 65 },
    [0x0C] = { 26, 26, 26 },   // bowgun
    [0x0F] = { 10, 10, 10 },   // sub machine gun
};

static int is_dog(const Enemy *e) { return types[e->type].type == TYPE_DOG; }

static void dog_anim(Enemy *e, int anim, int key) { set_anim(e, SET_GAIT, anim, key); }

// keeps the keyframe when only the gait changes (the PS1 writes the
// animation number alone, obj+0x14c)
static void dog_run_anim(Enemy *e, int delta)
{
    int want = delta < 0 ? D_RUN_LEFT : delta > 0 ? D_RUN_RIGHT : D_RUN;
    if (e->anim != types[e->type].set_start[SET_GAIT] + want)
        dog_anim(e, want, e->key);
}

static void dog_speed_up(Enemy *e, int by, int max)
{
    if (e->speed < max && (e->speed += by) > max)
        e->speed = max;
}

static void dog_slow_down(Enemy *e, int by, int min)
{
    if (e->speed > min && (e->speed -= by) < min)
        e->speed = min;
}

// how far to turn toward (x, z), at most rate RE2 units (FUN_80014fb4); DS units
static int turn_delta(const Enemy *e, int32_t x, int32_t z, int rate)
{
    int diff = ((angle_to(e, x, z) - e->dir + 16384) & 32767) - 16384;
    int step = rate * 8;
    return diff > step ? step : diff < -step ? -step : diff;
}

// dist along dir (DS units) against the room's walls; 1 if it got all the way
static int move_dir(Enemy *e, int i, int16_t dir, int32_t dist)
{
    int32_t mx = (dist * cosLerp(dir)) >> 12, mz = -((dist * sinLerp(dir)) >> 12);
    int moved = 1;
    if (!game_blocked(e->x, e->z, e->x + mx, e->z, e->floor_y) && !crowded(i, e->x + mx, e->z))
        e->x += mx;
    else
        moved = 0;
    if (!game_blocked(e->x, e->z, e->x, e->z + mz, e->floor_y) && !crowded(i, e->x, e->z + mz))
        e->z += mz;
    else
        moved = 0;
    return moved;
}

// in the air: falls; 1 when it lands
static int dog_fall(Enemy *e)
{
    e->y += e->vspeed;
    e->vspeed += DOG_GRAVITY;
    if (e->vspeed > 0 && e->y >= e->floor_y) {
        e->y = e->floor_y;
        e->airborne = 0;
        return 1;
    }
    return 0;
}

static void dog_enter(Enemy *e, int state)
{
    e->state = state;
    e->sub = 0;
}

static void dog_spawn(Enemy *e)
{
    e->hp = dog_hp[rnd() & 15] + (rnd() & 3);
    e->floor_y = e->y;
    // start state by spawn variant (init 0x801000f4, table 0x8010527c)
    switch (e->flags & EF_VARIANT) {
    case 1: dog_anim(e, D_WALK, rnd() & 0x3F); dog_enter(e, DOG_CIRCLE); break;
    case 2: case 6: dog_anim(e, D_RUN, rnd() & 15); dog_enter(e, DOG_HUNT); break;
    default: dog_anim(e, D_WALK, rnd() & 0x3F); dog_enter(e, DOG_IDLE); break;
    }
}

static int same_floor(const Enemy *e, const EnemyTarget *p) { return abs(p->y - e->floor_y) < 1000; }

// state 0: walks about at 30 a tick, now and then stops to sniff for 90 ticks
// (0x801004cc; the PS1 walks between the room's waypoints); wakes when Leon
// comes within 4000 on its floor or another dog has attacked, hunts at a shot
static void dog_idle(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (same_floor(e, p) && (d < 4000 || dog_alert || (p->noise & NOISE_SHOT))) {
        dog_enter(e, (p->noise & NOISE_SHOT) ? DOG_HUNT : DOG_CIRCLE);
        return;
    }
    switch (e->sub) {
    case 0:
        e->speed = 30;
        e->timer = rnd() + 60;
        dog_anim(e, D_WALK, e->key);
        e->sub = 1;
        // fall through
    case 1:
        anim_step(e, NULL);
        if (!move_dir(e, i, e->dir, e->speed) || e->timer-- == 0) {
            if (!(rnd() & 3)) { // sniffs
                e->sub = 2;
                e->timer = 90;
                dog_anim(e, D_SNIFF, 0);
            } else {            // another way
                e->sub = 3;
                e->turned = (int16_t)(((rnd() << 7) - 16384) | 1);
            }
        }
        break;
    case 2:
        anim_step(e, NULL);
        if (e->timer-- == 0)
            e->sub = 0;
        break;
    case 3: { // turns 16 a tick (FUN_80014ef8), walking on
        int step = e->turned > 0 ? 128 : -128;
        if (abs(e->turned) <= 128) {
            step = e->turned;
            e->sub = 0;
        }
        e->dir = (e->dir + step) & 32767;
        e->turned -= step;
        anim_step(e, NULL);
        move_dir(e, i, e->dir, e->speed);
        break;
    }
    }
}

// state 1: runs at a point up to 1024 beside Leon, turning 16 a tick. Within
// 3000 its aggression rises by 1 a tick (21 while he runs); with him straight
// ahead (+-11 degrees) and more than 600 away it jumps. Above 64, at a shot or
// when another dog attacked: hunts.
static void dog_circle(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (e->sub == 0) {
        e->sub = 1;
        e->speed = 30;
        e->aggr = 0;
        e->circ_x = dog_offset[rnd() & 7];
        e->circ_z = dog_offset[rnd() & 7];
        dog_anim(e, D_WALK, e->key);
    }
    e->dir = (e->dir + turn_delta(e, p->x + e->circ_x, p->z + e->circ_z, 16)) & 32767;
    move_dir(e, i, e->dir, e->speed);
    anim_step(e, NULL);
    int ahead = in_cone(e, p->x, p->z, 0x80);
    if (d < 3000) {
        e->aggr += (p->noise & NOISE_RUN) ? 21 : 1;
        if (ahead && d > 600 && p->can_grab && same_floor(e, p)) {
            dog_enter(e, DOG_ATTACK);
            return;
        }
    } else {
        if (e->aggr)
            e->aggr--;
        if (ahead && (p->noise & NOISE_RUN))
            e->aggr += 20;
    }
    if (e->aggr > 64 || (p->noise & NOISE_SHOT) || dog_alert)
        dog_enter(e, DOG_HUNT);
}

// Can it jump at Leon now (FUN_80104b6c)? 1 yes; -1 he is not ahead
// (+-22.5 degrees); -2 he is dead; -4 he is ahead but cannot be bitten now
// (held, hurt); 0 too far or on another floor.
static int dog_can_attack(const Enemy *e, const EnemyTarget *p, uint32_t d)
{
    if (p->dead)
        return -2;
    if (d >= 4000 || !same_floor(e, p))
        return 0;
    if (!in_cone(e, p->x, p->z, 0x100))
        return -1;
    return p->can_grab ? 1 : -4;
}

// state 2: runs at Leon, faster up to 240 a tick, turning up to 96 a tick.
// Every tick he is not ahead counts 10; past 450 it slows down, turns round
// (180 degrees, drifting on) and runs again (sub 2, 0x80100dd0).
static void dog_hunt(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    int32_t gx = e->has_goal ? e->goal_x : p->x, gz = e->has_goal ? e->goal_z : p->z;
    int delta = 0;
    switch (e->sub) {
    case 0:
        e->sub = 1;
        e->turn_rate = 0x60;
        e->misses = 0;
        dog_run_anim(e, 0);
        // fall through
    case 1: {
        dog_speed_up(e, 40, 240);
        if (e->cooldown)
            e->cooldown--;
        else
            delta = turn_delta(e, gx, gz, e->turn_rate);
        e->dir = (e->dir + delta) & 32767;
        move_dir(e, i, e->dir, e->speed);
        dog_run_anim(e, delta);
        anim_step(e, NULL);
        int r = dog_can_attack(e, p, d);
        if (r == 1) {
            dog_enter(e, DOG_ATTACK);
            return;
        }
        if (r == -2) {
            dog_enter(e, DOG_IDLE);
            return;
        }
        if (r < 0)
            e->misses += 10;
        if (e->misses > 450 || r == -4) {
            if (p->hp > 20)
                e->misses = 0;
            e->sub = 2;
            e->timer2 = 1; // slowing down
            e->turned = 0;
        }
        break;
    }
    case 2: // U-turn
        if (e->timer2 == 1) {
            dog_slow_down(e, 6, 10);
            if (e->speed < 11)
                e->timer2 = 2;
        } else {
            dog_speed_up(e, 6, 240);
        }
        delta = turn_delta(e, gx, gz, 0x60);
        move_dir(e, i, (e->dir - e->turned) & 32767, e->speed); // drifts on
        e->turned += delta;
        e->dir = (e->dir + delta) & 32767;
        dog_run_anim(e, delta);
        anim_step(e, NULL);
        if (abs(e->turned) > 0x7FF * 8 || delta == 0)
            e->sub = 1;
        break;
    }
}

// The bite (FUN_80104c84): Leon within 1000 of its head (700 when he is down
// to 20 HP or less) on its floor, while it rises.
static int dog_bites(Enemy *e, const EnemyTarget *p)
{
    int32_t hx = e->x + ((600 * cosLerp(e->dir)) >> 12), hz = e->z - ((600 * sinLerp(e->dir)) >> 12);
    float dx = (float)(p->x - hx), dz = (float)(p->z - hz);
    int range = p->hp > 20 ? 1000 : 700;
    if (!p->can_grab || !same_floor(e, p) || dx * dx + dz * dz >= (float)range * range)
        return 0;
    game_blood(0, p->x, p->y - 1400, p->z, e->dir, 20, p->y);
    int r = game_player_damage(DOG_BITE);
    if (r == 2) {
        game_player_killed();
    } else {
        int behind = ((p->dir - e->dir + 8192) & 32767) < 16384; // FUN_800152b0
        game_player_hurt(behind ? 2 : 1);
    }
    return 1;
}

// state 3: crouches 8 ticks moving 65 a tick, then leaps (280 forward, 280 up,
// gravity 40); bites on the way up. Missed: lands and hunts again. Bit:
// bounces off backward, turning round, and pauses before the next run.
static void dog_attack(Enemy *e, int i, const EnemyTarget *p)
{
    switch (e->sub) {
    case 0: // FUN_80101304
        e->sub = 1;
        e->timer = 8;
        e->speed = 65;
        e->vspeed = -280;
        dog_anim(e, D_JUMP, 0);
        // fall through
    case 1:
        move_dir(e, i, e->dir, e->speed);
        anim_step(e, NULL);
        if (--e->timer == 0) {
            e->sub = 2;
            e->speed = 280;
            e->airborne = 1;
            dog_alert = 1;
            sound_at(e, SND_DOG_JUMP);
        }
        break;
    case 2: // in the air (0x801013ec)
        move_dir(e, i, e->dir, e->speed);
        if (e->vspeed < 0 && dog_bites(e, p)) {
            e->sub = 5;
            e->turned = 0;
            e->speed >>= 2;
            dog_anim(e, D_BOUNCE, 0);
            break;
        }
        if (e->vspeed < 0) {
            e->y += e->vspeed;
            e->vspeed += DOG_GRAVITY;
            anim_step(e, NULL);
            break;
        }
        e->sub = 3; // missed (0x80101348)
        e->misses += 20;
        dog_anim(e, D_LAND, 0);
        // fall through
    case 3: // comes down (FUN_801014d8)
        move_dir(e, i, e->dir, e->speed);
        anim_step(e, NULL);
        if (dog_fall(e)) {
            sound_at(e, SND_DOG_LAND);
            e->sub = 4;
        }
        break;
    case 4:
        dog_slow_down(e, 64, 0);
        move_dir(e, i, e->dir, e->speed);
        if (anim_step(e, NULL))
            dog_enter(e, DOG_HUNT);
        break;
    case 5: // bounces off (0x801015e8)
        move_dir(e, i, (e->dir + 16384) & 32767, e->speed);
        if (e->airborne && dog_fall(e)) {
            sound_at(e, SND_DOG_LAND);
            e->sub = 6;
        }
        if (e->turned < 0x800 * 8) {
            e->dir = (e->dir + 0x80 * 8) & 32767;
            e->turned += 0x80 * 8;
        }
        if (e->key < 0x11)
            anim_step(e, NULL);
        else
            e->speed = 0;
        break;
    case 6:
        if (e->turned < 0x800 * 8) {
            e->dir = (e->dir + 0x80 * 8) & 32767;
            e->turned += 0x80 * 8;
        }
        if (anim_step(e, NULL)) {
            dog_enter(e, DOG_HUNT);
            e->cooldown = dog_pause[rnd() & 7];
        }
        break;
    }
}

// Shot (0x801031d8): thrown back along Leon's facing (240 a tick, 10 less
// every tick; half that and falling first when hit in the air), lies, gets up
// and hunts. Hit again meanwhile it only bleeds. Killed: the same throw,
// then it stays down.
static void dog_shot(Enemy *e, int16_t player_dir, int dying)
{
    if (e->state == DOG_HURT || e->state == ST_DYING) {
        if (dying && e->state != ST_DYING) {
            e->state = ST_DYING;
            if (e->sub == 4) { // was getting up: down again
                dog_anim(e, D_DOWN, 0);
                e->sub = 3;
            }
            sound_at(e, SND_DOG_DIE);
        }
        return;
    }
    e->push_dir = (player_dir + dog_throw[rnd() & 7] * 8) & 32767;
    if (e->airborne) {
        e->speed >>= 1;
        e->sub = 1;
    } else {
        e->speed = 240;
        e->sub = 2;
    }
    e->state = dying ? ST_DYING : DOG_HURT;
    dog_anim(e, D_THROWN, 0);
    sound_at(e, dying ? SND_DOG_DIE : SND_DOG_HIT);
}

static void dog_thrown(Enemy *e, int i)
{
    switch (e->sub) {
    case 1: // falls (0x8010335c)
        move_dir(e, i, e->push_dir, e->speed);
        anim_step(e, NULL);
        if (dog_fall(e)) {
            sound_at(e, SND_DOG_LAND_HARD);
            e->sub = 2;
        }
        break;
    case 2: // slides (0x8010347c)
        move_dir(e, i, e->push_dir, e->speed);
        dog_slow_down(e, 10, 0);
        if (anim_step(e, NULL)) {
            dog_anim(e, D_DOWN, 0);
            e->sub = 3;
        }
        break;
    case 3: // down
        if (e->speed) {
            move_dir(e, i, e->push_dir, e->speed);
            dog_slow_down(e, 10, 0);
        }
        if (e->key + 1 >= types[e->type].model.anims[e->anim].count) {
            if (e->state == ST_DYING) {
                e->state = ST_DEAD;
                return;
            }
            dog_anim(e, D_GET_UP, 0);
            e->sub = 4;
        } else {
            anim_step(e, NULL);
        }
        break;
    case 4: // gets up
        if (anim_step(e, NULL)) {
            e->speed = 0;
            dog_enter(e, DOG_HUNT);
        }
        break;
    }
}

// Paws on the floor: the EDD flag 0x40000 of these keyframes plays sound 8
// (em20 entry 0x80100460): walk, run (straight / turning), anim 9, the leap
static const uint8_t dog_steps[][3] = {
    { D_WALK, 5, 39 }, { D_RUN, 1, 15 }, { D_RUN_LEFT, 1, 15 }, { D_RUN_RIGHT, 1, 15 }, { 9, 11, 22 }, { D_JUMP, 2, 2 },
};
#define SND_DOG_STEP 8

static void dog_step_sound(const Enemy *e, int anim, int key)
{
    int n = anim - types[e->type].set_start[SET_GAIT];
    for (unsigned k = 0; k < sizeof(dog_steps) / sizeof(dog_steps[0]); k++)
        if (dog_steps[k][0] == n && (key == dog_steps[k][1] || key == dog_steps[k][2])) {
            sound_at(e, SND_DOG_STEP);
            return;
        }
}

static void dog_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    int anim = e->anim, key = e->key;
    dog_step_sound(e, anim, key);
    switch (e->state) {
    case DOG_IDLE: dog_idle(e, i, p, d); break;
    case DOG_CIRCLE: dog_circle(e, i, p, d); break;
    case DOG_HUNT: dog_hunt(e, i, p, d); break;
    case DOG_ATTACK: dog_attack(e, i, p); break;
    case DOG_HURT:
    case ST_DYING: dog_thrown(e, i); break;
    default: anim_step(e, NULL); break;
    }
}

// ---------------------------------------------------------------- crow
// Crow (type 0x21; overlay slot 1 at 0x8010d000, think / move tables
// 0x801118c4 / 0x80111900 by state; docs/enemy-others.md). 10 HP, so every
// hit kills it. It flies 1800..4800 above Leon's feet, dives at him and pecks
// until he shakes it off. One crow attacks at a time (0x800c77f0 & 1).
#define TYPE_CROW 0x21
#define CROW_PERCH 0            // sits, wakes when Leon comes near (0x8010d6c0 / 0x8010d7a0)
#define CROW_TAKEOFF 2          // (0x8010dc34)
#define CROW_FLY 4              // flies about over him (0x8010e0ac / 0x8010e1b0, climbs 5, sinks 6)
#define CROW_DIVE 13            // dives at his head (0x8010f1d0 / 0x8010f244)
#define CROW_ON 14              // on him, pecks (0x8010f4ec / 0x8010f5b0)
#define CROW_FALL 10            // hit or dead: falls to the floor (0x8010efb0)
#define C_PERCH 0x00
#define C_GLIDE 0x03
#define C_FLAP 0x04
#define C_PECK 0x05
#define C_TAKEOFF 0x07
#define C_HIT 0x09
#define C_DOWN 0x0B
#define CROW_PECK 5             // x1.5 (FUN_8003cf38): 7 HP
#define CROW_SHAKE 100          // -1 per tick, -3 more per key pressed (FUN_8001532c)
#define SND_CROW_CAW 1
#define SND_CROW_PECK 2
#define SND_CROW_HIT 3
#define SND_CROW_FLOOR 5
#define SND_CROW_PERCHED 6


static int is_crow(const Enemy *e) { return types[e->type].type == TYPE_CROW; }

static void crow_anim(Enemy *e, int anim, int key) { set_anim(e, SET_GAIT, anim, key); }

static void crow_spawn(Enemy *e)
{
    e->hp = 10;
    e->floor_y = e->y;
    crow_anim(e, C_PERCH, rnd() & 7);
    dog_enter(e, CROW_PERCH);
}

static void crow_release(Enemy *e, int i)
{
    if (crow_lock == i)
        crow_lock = -1;
    e->cooldown = 30; // 0x21c: no new attack for a while
}

// flies along its facing at its speed, rising / sinking by vspeed
static void crow_move(Enemy *e, int i)
{
    if (!move_dir(e, i, e->dir, e->speed))
        e->dir = (e->dir + 0x400 * 8 + (rnd() << 5)) & 32767; // bumped a wall: turns away
    e->y += e->vspeed;
}

static void crow_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    e->floor_y = p->y; // it lands on Leon's floor
    int32_t gx = e->has_goal ? e->goal_x : p->x, gz = e->has_goal ? e->goal_z : p->z;
    switch (e->state) {
    case CROW_PERCH:
        if (e->sub == 0) { // waiting to fly off (state 1: 0..7 ticks)
            if (anim_step(e, NULL) && !(rnd() & 3)) {
                crow_anim(e, rnd() & 1 ? C_PERCH : 1, 0);
                if (!(rnd() & 1))
                    sound_at(e, SND_CROW_PERCHED);
            }
            if (d < 1800 || ((p->noise & NOISE_RUN) && d < 5400) || (p->noise & NOISE_SHOT)) {
                e->sub = 1;
                e->timer = rnd() & 7;
            }
        } else if (e->timer-- <= 0) {
            dog_enter(e, CROW_TAKEOFF);
        } else {
            anim_step(e, NULL);
        }
        break;
    case CROW_TAKEOFF:
        if (e->sub == 0) {
            int k = rnd() & 7;
            if (rnd() & 1)
                sound_at(e, SND_CROW_CAW);
            crow_anim(e, C_TAKEOFF, k);
            e->speed = (rnd() & 0x1F) + 100;
            e->vspeed = -10;
            e->timer = 9 - k;
            e->sub = 1;
        }
        if (--e->timer <= 0) { // climbs in steps (0x8010dc34 cases 1-4)
            static const int8_t climb[4][2] = { { -80, 6 }, { -60, 4 }, { -100, 9 }, { -80, 127 } };
            int s = e->sub - 1 < 4 ? e->sub - 1 : 3;
            e->vspeed = climb[s][0];
            e->timer = climb[s][1];
            if (e->sub < 5)
                e->sub++;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x20)) & 32767;
        crow_move(e, i);
        if (e->speed < 300)
            e->speed++;
        if (anim_step(e, NULL))
            dog_enter(e, CROW_FLY);
        break;
    case CROW_FLY:
        if (e->sub == 0) { // glides or flaps for 0..31 ticks (0x8010e1b0)
            int flap = rnd() & 1;
            crow_anim(e, flap ? C_FLAP : C_GLIDE, e->key);
            e->aggr = flap ? 3 : 2; // speed gain
            e->timer = rnd() & 0x1F;
            e->sub = 1;
            if (!(rnd() & 7))
                sound_at(e, SND_CROW_CAW);
        }
        // height band (states 5 / 6): 1800..4800 above his feet
        if (e->y > p->y - 1800)
            e->vspeed = -40;
        else if (e->y < p->y - 4800)
            e->vspeed = 40;
        else if (!(rnd() & 15))
            e->vspeed = (rnd() & 1) ? 40 : -40;
        if (p->dead) { // circles a little
            e->dir = (e->dir + 64) & 32767;
        } else if (e->sub == 2) { // sharp turn, slowing down (state 8, 0x8010e964)
            e->dir = (e->dir + turn_delta(e, gx, gz, 0x60)) & 32767;
            if (in_cone(e, gx, gz, 0x60) || e->timer2-- == 0)
                e->sub = 0;
        } else {
            e->dir = (e->dir + turn_delta(e, gx, gz, 0x20)) & 32767;
            // circling him wide (FUN_801111bc counts it in obj+0x21e): turn sharply
            if (in_cone(e, p->x, p->z, 0x100))
                e->misses = 0;
            else if (++e->misses > 10) {
                e->misses = 0;
                e->sub = 2;
                e->timer2 = rnd() & 0x3F;
            }
        }
        crow_move(e, i);
        if (e->sub == 2) {
            if (e->speed > 100)
                e->speed -= 4;
        } else if (e->speed < 300) {
            e->speed += e->aggr;
        }
        anim_step(e, NULL);
        if (e->sub == 1 && e->timer-- == 0)
            e->sub = 0;
        if (e->cooldown) {
            e->cooldown--;
            break;
        }
        // attack (FUN_801111bc): 900..8100 away, in sight, ahead +-22.5 degrees
        if (!p->dead && crow_lock < 0 && d >= 900 && d < 8100 && e->sees && in_cone(e, p->x, p->z, 0x100)) {
            crow_lock = i;
            dog_enter(e, CROW_DIVE);
        }
        break;
    case CROW_DIVE: { // at his head (0x8010f244), turns fast
        int32_t head = p->y - 2880;
        if (e->sub == 0) {
            crow_anim(e, C_FLAP, 0);
            e->speed = 0;
            e->vspeed = 0;
            e->timer = 0;
            e->sub = 1;
        }
        if (e->timer-- <= 0) {
            sound_at(e, SND_CROW_CAW);
            e->timer = 14;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x180)) & 32767;
        if (e->speed < 300)
            e->speed += 12;
        if (d < 900 && e->speed > 100)
            e->speed = 100;
        if (e->y < head - 60)
            e->y += 60;
        else if (e->y > head + 60)
            e->y -= 60;
        anim_step(e, NULL);
        if (p->dead || !in_cone(e, p->x, p->z, 0xC0) || !e->sees || !p->can_grab) {
            crow_release(e, i);
            dog_enter(e, CROW_FLY);
            break;
        }
        if (d < 650 && abs(e->y - head) < 99) {
            dog_enter(e, CROW_ON);
            break;
        }
        move_dir(e, i, e->dir, e->speed);
        break;
    }
    case CROW_ON: // pecks (td0ac): every 2..9 ticks; shaken off when the count runs out
        if (e->sub == 0) {
            crow_anim(e, C_PECK, 0);
            e->turned = CROW_SHAKE;
            e->timer = 4;
            e->sub = 1;
        }
        // sits on his head, facing his back
        e->x = p->x + ((250 * cosLerp(p->dir)) >> 12);
        e->z = p->z - ((250 * sinLerp(p->dir)) >> 12);
        e->y = p->y - 2400;
        e->dir = (p->dir + 16384) & 32767;
        anim_step(e, NULL);
        e->turned -= 1 + (p->pressed ? 3 : 0);
        if (e->timer-- <= 0) {
            game_blood(0, p->x, p->y - 2300, p->z, e->dir, 15, p->y);
            sound_at(e, SND_CROW_PECK);
            int r = game_player_damage(CROW_PECK);
            if (r == 2)
                game_player_killed();
            else
                game_player_hurt(0);
            e->timer = (rnd() & 7) + 2;
            if (r)
                e->turned = -1;
        }
        if (e->turned < 0 || p->dead) { // flies off
            crow_release(e, i);
            e->speed = 150;
            e->vspeed = -60;
            e->dir = (p->dir + 16384 + (rnd() << 5) - 4096) & 32767;
            dog_enter(e, CROW_FLY);
        }
        break;
    case CROW_FALL:
    case ST_DYING: // falls to the floor (0x8010efb0 cases 1-3), lies
        if (e->sub == 0) {
            crow_anim(e, C_HIT, 0);
            e->vspeed = 0;
            e->sub = 1;
        }
        if (e->sub == 1) {
            anim_step(e, NULL);
            e->vspeed += 20;
            e->y += e->vspeed;
            move_dir(e, i, e->push_dir, e->speed);
            if (e->y >= e->floor_y - 250) {
                e->y = e->floor_y - 250;
                sound_at(e, SND_CROW_FLOOR);
                crow_anim(e, C_DOWN, 0);
                e->timer = rnd() & 15;
                e->sub = 2;
            }
        } else if (e->state == ST_DYING) { // 11 keys of the flutter, then still (0x8010f900 case 5)
            if (e->key >= 11)
                e->state = ST_DEAD;
            else
                anim_step(e, NULL);
        } else if (anim_step(e, NULL)) { // stunned: up again
            dog_enter(e, CROW_TAKEOFF);
        }
        break;
    default:
        anim_step(e, NULL);
        break;
    }
}

// Hit (0x8010f900): knocked away from Leon at 100 a tick, falls (gravity 20)
static void crow_shot(Enemy *e, int i, int16_t dir)
{
    if (crow_lock == i)
        crow_lock = -1;
    e->state = ST_DYING;
    e->sub = 0;
    e->speed = 100;
    e->push_dir = dir;
    sound_at(e, SND_CROW_HIT);
}

// ---------------------------------------------------------------- licker
// Licker (types 0x22 and the tougher 0x24; overlay of CDEMD0.EMS at
// 0x80100000, states in table 0x80107488; docs/enemy-others.md). It is blind
// and goes by sound (FUN_80100db0: any step within 3000, running or a shot
// within 9000). Attacks: the claw (state 9), the tongue (state 10) and the
// jump (state 11). Only one licker claws at a time (0x8010752c).
// The PS1 lets them crawl on ceilings and walls (spawn status bits 8..11);
// here they stay on the floor.
#define TYPE_LICKER 0x22
#define TYPE_LICKER2 0x24
#define LICK_IDLE 0             // listens (state 0, 0x801017c8)
#define LICK_TURN 2             // turns to the sound (0x801019a4)
#define LICK_STALK 6            // crawls at him (0x80101efc / 0x8010291c)
#define LICK_CLAW 9             // (0x801020bc)
#define LICK_TONGUE 10          // (0x801023c4)
#define LICK_JUMP 11            // (0x80103cec)
#define LICK_HURT 12            // flinches, pushed back (0x8010574c)
#define LICK_CLAW_DAMAGE 25     // x1.5: 37 HP
#define LICK_TONGUE_DAMAGE 15   // x1.5: 22 HP
#define SND_LICK_CLAW 5
#define SND_LICK_HIT 7
#define SND_LICK_JUMP 8
#define SND_LICK_JUMP_HIT 10

enum { LA_CRAWL, LA_TURN, LA_IDLE, LA_LISTEN, LA_CROUCH, LA_CLAW, LA_TONGUE, LA_HIT, LA_DIE, LA_DEAD,
       LA_JUMP, LA_AIR, LA_LAND, LA_COUNT };
// animations of type 0x22 and 0x24 (the second lacks some of the first's)
static const uint8_t lick_anims[2][LA_COUNT] = {
    { 1, 2, 3, 4, 6, 7, 21, 22, 28, 11, 31, 32, 33 },
    { 1, 2, 3, 4, 6, 7, 15, 16, 20, 11, 23, 24, 25 },
};
// health (0x80107124)
static const uint8_t lick_hp[16] = { 97, 111, 91, 85, 120, 125, 109, 111, 111, 109, 91, 111, 125, 120, 97, 111 };
// the claw's reach by keyframe 17..24 (0x80107382)
static const uint16_t claw_reach[8] = { 2500, 3500, 4600, 4500, 4200, 3500, 3000, 2500 };
// damage by weapon (near / middle / far), 0 = the weapon's own
static const uint16_t lick_damage[2][20][3] = {
    { [2] = { 14, 13, 12 }, [3] = { 14, 13, 12 }, [4] = { 14, 13, 12 }, [5] = { 200, 200, 200 },
      [6] = { 200, 200, 200 }, [7] = { 50, 45, 40 }, [8] = { 110, 95, 80 }, [0x0F] = { 11, 11, 11 } },
    { [2] = { 10, 9, 8 }, [3] = { 10, 9, 8 }, [4] = { 10, 9, 8 }, [5] = { 40, 40, 40 },
      [6] = { 40, 40, 40 }, [7] = { 30, 27, 25 }, [8] = { 50, 47, 45 }, [0x0F] = { 8, 8, 8 } },
};

static int is_licker(const Enemy *e) { int t = types[e->type].type; return t == TYPE_LICKER || t == TYPE_LICKER2; }
static int lick_kind(const Enemy *e) { return types[e->type].type == TYPE_LICKER2; }
static void lick_anim(Enemy *e, int which, int key) { set_anim(e, SET_GAIT, lick_anims[lick_kind(e)][which], key); }

static void licker_spawn(Enemy *e)
{
    e->hp = lick_hp[rnd() & 15];
    if (lick_kind(e))
        e->hp -= 20;
    e->floor_y = e->y;
    e->airborne = ((e->flags >> 8) & 0xF) != 0; // on the ceiling or a wall: drops to the floor
    lick_anim(e, LA_IDLE, rnd() & 0x3F);
    dog_enter(e, LICK_IDLE);
}

// heard him? (FUN_80100db0)
static int licker_hears(const EnemyTarget *p, uint32_t d)
{
    if (p->dead)
        return 0;
    if (d < 3000)
        return (p->noise & (NOISE_WALK | NOISE_RUN | NOISE_SHOT)) != 0;
    return d < 9000 && (p->noise & (NOISE_RUN | NOISE_SHOT));
}

static void licker_hits(Enemy *e, const EnemyTarget *p, int damage)
{
    game_blood(0, p->x, p->y - 1500, p->z, e->dir, 25, p->y);
    int r = game_player_damage(damage);
    if (r == 2) {
        game_player_killed();
    } else {
        int behind = ((p->dir - e->dir + 8192) & 32767) < 16384;
        game_player_hurt(behind ? 2 : 1);
    }
    e->cooldown = 40; // obj+0x22e: no claw for a while
    e->timer2 = 1;    // this attack has hit
}

// root motion of the crawl / attack animations
static void licker_step(Enemy *e, int i)
{
    int32_t fwd;
    int done = anim_step(e, &fwd);
    (void)done;
    move_dir(e, i, e->dir, fwd);
}

static void licker_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (e->airborne && e->state < LICK_HURT) { // drops down to his floor first
        e->floor_y = p->y;
        e->y += 120;
        if (e->y >= e->floor_y) {
            e->y = e->floor_y;
            e->airborne = 0;
        }
        anim_step(e, NULL);
        return;
    }
    if (e->cooldown)
        e->cooldown--;
    int last = e->key + 1 >= types[e->type].model.anims[e->anim].count;
    switch (e->state) {
    case LICK_IDLE:
        anim_step(e, NULL);
        if (licker_hears(p, d))
            dog_enter(e, LICK_TURN);
        break;
    case LICK_TURN: // to him, 0x60 a tick
        if (e->sub == 0) {
            lick_anim(e, LA_TURN, 0);
            e->sub = 1;
            e->timer = 0;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x60)) & 32767;
        anim_step(e, NULL);
        if (in_cone(e, p->x, p->z, 0x80))
            dog_enter(e, LICK_STALK);
        else if (++e->timer > 120 || p->dead)
            dog_enter(e, LICK_IDLE);
        break;
    case LICK_STALK:
        if (e->sub == 0) {
            lick_anim(e, LA_CRAWL, e->key);
            e->sub = 1;
            e->timer = 0;
        }
        e->dir = (e->dir + turn_delta(e, e->has_goal ? e->goal_x : p->x, e->has_goal ? e->goal_z : p->z,
                                       d < 4000 ? 0x40 : 0x10)) & 32767;
        licker_step(e, i);
        if (p->dead) {
            dog_enter(e, LICK_IDLE);
            break;
        }
        if (++e->timer < 8)
            break; // crawls a little before it decides
        if (d < 2000 && !e->cooldown && claw_lock < 0 && p->can_grab) {
            dog_enter(e, in_cone(e, p->x, p->z, 0x300) ? LICK_CLAW : LICK_TURN);
        } else if (d >= 2000 && d < 2800 && p->can_grab && in_cone(e, p->x, p->z, 0x154) && !(rnd() & 15)) {
            dog_enter(e, LICK_TONGUE);
        } else if (d >= 3500 && d < 6000 && p->can_grab && in_cone(e, p->x, p->z, 0x100) && !(rnd() & 31)) {
            dog_enter(e, LICK_JUMP);
        }
        break;
    case LICK_CLAW: // keys 17..24 reach the table's distance, +-22.5 degrees
        if (e->sub == 0) {
            claw_lock = i;
            lick_anim(e, LA_CLAW, 0);
            e->sub = 1;
            e->timer2 = 0;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 8)) & 32767;
        licker_step(e, i);
        if (e->key == 17)
            sound_at(e, SND_LICK_CLAW);
        if (!e->timer2 && e->key >= 17 && e->key <= 24 && p->can_grab && in_cone(e, p->x, p->z, 0x100) &&
            d < claw_reach[e->key - 17] - 1500) // reach counts from its hips, Leon from his feet
            licker_hits(e, p, LICK_CLAW_DAMAGE);
        if (last) {
            claw_lock = -1;
            dog_enter(e, LICK_STALK);
        }
        break;
    case LICK_TONGUE: // keys 1..5: within 2800, +-30 degrees
        if (e->sub == 0) {
            lick_anim(e, LA_TONGUE, 0);
            e->sub = 1;
            e->timer2 = 0;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 8)) & 32767;
        licker_step(e, i);
        if (!e->timer2 && e->key >= 1 && e->key <= 5 && p->can_grab && d < 2800 && in_cone(e, p->x, p->z, 0x154))
            licker_hits(e, p, LICK_TONGUE_DAMAGE);
        if (last)
            dog_enter(e, LICK_STALK);
        break;
    case LICK_JUMP:
        switch (e->sub) {
        case 0: // crouches
            lick_anim(e, LA_CROUCH, 0);
            e->sub = 1;
            e->timer2 = 0;
            break;
        case 1:
            if (anim_step(e, NULL)) {
                sound_at(e, SND_LICK_JUMP);
                lick_anim(e, LA_JUMP, 0);
                e->sub = 2;
            }
            break;
        case 2: // takes off, turning 6 a tick
            e->dir = (e->dir + turn_delta(e, p->x, p->z, 6)) & 32767;
            if (anim_step(e, NULL)) {
                lick_anim(e, LA_AIR, 0);
                e->speed = 550;
                e->vspeed = -245;
                e->sub = 3;
            }
            break;
        case 3: // in the air: 550 forward (-8 a tick), 245 up (+35 a tick)
            e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x20)) & 32767;
            move_dir(e, i, e->dir, e->speed);
            e->speed = e->speed > 8 ? e->speed - 8 : 0;
            e->y += e->vspeed;
            e->vspeed += 35;
            if (e->key + 1 < types[e->type].model.anims[e->anim].count)
                e->key++;
            if (!e->timer2 && e->key >= 11 && e->key <= 14 && d < 2200 && p->can_grab) {
                // 80, or 100 if he has more than 110; survives with at least 40
                int damage = p->hp > 110 ? 100 : 80;
                int left = p->hp - (damage < 41 ? damage + (damage >> 1) : damage);
                sound_at(e, SND_LICK_JUMP_HIT);
                if (left >= 0 && left < 40)
                    damage = p->hp - 40;
                licker_hits(e, p, damage);
            }
            if (e->vspeed > 0 && e->y >= e->floor_y) {
                e->y = e->floor_y;
                lick_anim(e, LA_LAND, 0);
                e->sub = 4;
            }
            break;
        case 4:
            if (anim_step(e, NULL))
                dog_enter(e, LICK_STALK);
            break;
        }
        break;
    case LICK_HURT: // pushed along the shot, 6 slower a tick
        move_dir(e, i, e->push_dir, e->speed);
        e->speed = e->speed > 6 ? e->speed - 6 : 0;
        if (e->y < e->floor_y) {
            e->y += 60;
            if (e->y > e->floor_y)
                e->y = e->floor_y;
        }
        if (anim_step(e, NULL)) // FUN_801055cc: back at him
            dog_enter(e, d < 2500 && in_cone(e, p->x, p->z, 0x140) ? LICK_STALK : LICK_TURN);
        break;
    case ST_DYING:
        if (e->sub == 0) {
            if (anim_step(e, NULL)) {
                lick_anim(e, LA_DEAD, 0);
                e->sub = 1;
            }
        } else if (last) {
            e->state = ST_DEAD;
        } else {
            anim_step(e, NULL);
        }
        if (e->y < e->floor_y) {
            e->y += 60;
            if (e->y > e->floor_y)
                e->y = e->floor_y;
        }
        break;
    default:
        anim_step(e, NULL);
        break;
    }
}

static void licker_shot(Enemy *e, int i, int16_t dir)
{
    if (claw_lock == i)
        claw_lock = -1;
    e->push_dir = dir;
    if (e->hp < 0) {
        sound_at(e, SND_LICK_JUMP);
        lick_anim(e, LA_DIE, 0);
        e->state = ST_DYING;
        e->sub = 0;
        return;
    }
    sound_at(e, SND_LICK_HIT);
    if (e->state == LICK_JUMP && e->sub == 3)
        return; // in the air it flies on
    lick_anim(e, LA_HIT, 0);
    e->speed = 100;
    dog_enter(e, LICK_HURT);
    e->sub = 1;
}

// ---------------------------------------------------------------- spider
// Giant spider (type 0x25; overlay slot 1 at 0x8010d000, floor states in
// table 0x801132e8; docs/enemy-others.md) and the small ones (0x26). The
// spider moves by its own speed (no root motion). On ceilings and walls
// (spawn variants 2..11) the PS1 lets it crawl there; here it drops to the
// floor when Leon comes near.
#define TYPE_SPIDER 0x25
#define TYPE_SPIDERLING 0x26
#define SP_IDLE 0               // decides (0x8010d708)
#define SP_WANDER 4             // (0x8010da00)
#define SP_TURN 5               // turns on the spot (0x8010db04)
#define SP_CHARGE 7             // crouches, aims, rushes, bites (0x8010dc64)
#define SP_SPIT 8               // spits poison (0x8010df0c)
#define SP_EVADE 9              // jumps aside (0x8010e198)
#define SP_HURT 10              // flinches (0x8010fcd8)
#define SP_BITE 20              // x1.5 (FUN_80112974): 30 HP
#define SP_SPIT_DAMAGE 5
#define SND_SPIDER_BITE 1

// health (0x801131cc) + rand & 3
static const uint8_t spider_hp[16] = { 109, 109, 109, 109, 129, 109, 109, 129, 109, 109, 109, 129, 109, 99, 109, 109 };

static int is_spider(const Enemy *e) { int t = types[e->type].type; return t == TYPE_SPIDER || t == TYPE_SPIDERLING; }
static int is_spiderling(const Enemy *e) { return types[e->type].type == TYPE_SPIDERLING; }

static void spider_spawn(Enemy *e)
{
    e->floor_y = e->y;
    if (is_spiderling(e)) {
        e->hp = 1;
        set_anim(e, SET_GAIT, 1, rnd() & 7);
    } else {
        e->hp = spider_hp[rnd() & 15] + (rnd() & 3);
        e->airborne = (e->flags & EF_VARIANT) >= 2 && (e->flags & EF_VARIANT) <= 11;
        set_anim(e, SET_GAIT, 0, rnd() & 15);
    }
    dog_enter(e, SP_IDLE);
}

static void spider_bites(Enemy *e, const EnemyTarget *p, int damage)
{
    sound_at(e, SND_SPIDER_BITE);
    game_blood(0, p->x, p->y - 1200, p->z, e->dir, 20, p->y);
    if (game_player_damage(damage) == 2)
        game_player_killed();
    else
        game_player_hurt(((p->dir - e->dir + 8192) & 32767) < 16384 ? 2 : 1);
}

// the small ones crawl at Leon and nip him when they touch
static void spiderling_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (e->state == ST_DYING) {
        if (anim_step(e, NULL))
            e->state = ST_DEAD;
        return;
    }
    if (e->cooldown)
        e->cooldown--;
    e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x40)) & 32767;
    anim_step(e, NULL);
    if (d > 500)
        move_dir(e, i, e->dir, 20);
    else if (!e->cooldown && p->can_grab) {
        e->cooldown = 30;
        if (game_player_damage(2) == 2)
            game_player_killed();
    }
}

static void spider_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    if (is_spiderling(e)) {
        spiderling_tick(e, i, p, d);
        return;
    }
    if (e->airborne) { // waits up there, drops when Leon comes near
        anim_step(e, NULL);
        if (d < 4000 || (p->noise & NOISE_SHOT)) {
            e->floor_y = p->y;
            e->y += 150;
            if (e->y >= e->floor_y) {
                e->y = e->floor_y;
                e->airborne = 0;
            }
        }
        return;
    }
    if (e->cooldown)
        e->cooldown--;
    int facing = in_cone(e, p->x, p->z, 0x80);
    switch (e->state) {
    case SP_IDLE:
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, 0, e->key);
            e->sub = 1;
        }
        anim_step(e, NULL);
        if (p->dead)
            break;
        if (p->noise & NOISE_SHOT) {
            dog_enter(e, (rnd() & 1) ? SP_TURN : SP_EVADE);
            break;
        }
        if (d < 8000 && !in_cone(e, p->x, p->z, 0x400)) {
            dog_enter(e, SP_TURN);
            break;
        }
        if (!e->cooldown && d < 7500 && facing && p->can_grab) {
            if (d > 1000 && d < 6000 && ((0x340 >> (rnd() & 15)) & 1))
                dog_enter(e, SP_SPIT);
            else
                dog_enter(e, SP_CHARGE);
        } else if (d >= 8000 && !(rnd() & 63)) {
            dog_enter(e, SP_WANDER);
        }
        break;
    case SP_WANDER: // anim 3, turning +-0x18 for 10..63 ticks
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, 3, 0);
            e->timer = (rnd() + 10) & 0x3F;
            e->turned = (rnd() & 1) ? 0x18 * 8 : -0x18 * 8;
            e->sub = 1;
        }
        e->dir = (e->dir + e->turned) & 32767;
        move_dir(e, i, e->dir, 40);
        anim_step(e, NULL);
        if (e->timer-- <= 0 || d < 8000)
            dog_enter(e, SP_IDLE);
        break;
    case SP_TURN: // anim 2, 0x80 a tick, 60..91 ticks at most
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, 2, 0);
            e->timer = (rnd() & 0x1F) + 60;
            e->sub = 1;
        }
        {
            int delta = turn_delta(e, p->x, p->z, 0x80);
            e->dir = (e->dir + delta) & 32767;
            anim_step(e, NULL);
            if (e->timer-- <= 0 || delta == 0)
                dog_enter(e, SP_IDLE);
        }
        break;
    case SP_CHARGE:
        switch (e->sub) {
        case 0: // crouches (anim 4), creeping on
            set_anim(e, SET_GAIT, 4, 0);
            e->sub = 1;
            // fall through
        case 1:
            move_dir(e, i, e->dir, 100);
            if (anim_step(e, NULL)) {
                set_anim(e, SET_GAIT, 5, 0);
                e->timer = (rnd() & 0x1F) + 20;
                e->sub = 2;
            }
            break;
        case 2: { // aims (0x40 a tick)
            int delta = turn_delta(e, p->x, p->z, 0x40);
            e->dir = (e->dir + delta) & 32767;
            anim_step(e, NULL);
            if (delta == 0) {
                e->timer = (rnd() & 0x1F) + 10;
                e->sub = 3;
            } else if (e->timer-- <= 0) {
                e->sub = 4;
            }
            break;
        }
        case 3: // rushes at 300, turning 0x10
            e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x10)) & 32767;
            move_dir(e, i, e->dir, 300);
            anim_step(e, NULL);
            if (d < 1500 && p->can_grab && abs(p->y - e->y) < 0x10CC) {
                spider_bites(e, p, SP_BITE);
                e->sub = 4;
            } else if (e->timer-- <= 0 || !in_cone(e, p->x, p->z, 0x200)) {
                e->sub = 4;
            }
            break;
        case 4: // stops (anim 7)
            set_anim(e, SET_GAIT, 7, 0);
            e->sub = 5;
            // fall through
        case 5:
            move_dir(e, i, e->dir, 100);
            if (anim_step(e, NULL)) {
                e->cooldown = 15;
                dog_enter(e, SP_IDLE);
            }
            break;
        }
        break;
    case SP_SPIT: // anim 11; the spit flies at him (FUN_80112828)
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, 11, 0);
            e->sub = 1;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x10)) & 32767;
        if (e->key == 20 && d < 6000 && in_cone(e, p->x, p->z, 0x100) && p->can_grab) {
            game_blood(1, e->x, e->y - 300, e->z, e->dir, 40, e->floor_y);
            spider_bites(e, p, SP_SPIT_DAMAGE);
        }
        if (anim_step(e, NULL)) {
            e->cooldown = 15;
            dog_enter(e, SP_IDLE);
        }
        break;
    case SP_EVADE: // jumps aside at 400..463 (anim 8)
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, 8, 0);
            e->push_dir = (angle_to(e, p->x, p->z) + (rnd() << 3) + ((rnd() & 1) ? 0x800 * 8 : 0) + 0x380 * 8) & 32767;
            e->speed = (rnd() & 0x3F) + 400;
            e->sub = 1;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 0x60)) & 32767;
        move_dir(e, i, e->push_dir, e->speed);
        if (anim_step(e, NULL))
            dog_enter(e, SP_IDLE);
        break;
    case SP_HURT: // anim 1, then jumps aside or decides again
        if (anim_step(e, NULL))
            dog_enter(e, (rnd() & 1) ? SP_EVADE : SP_IDLE);
        break;
    case ST_DYING: // anim 12, the legs curl up
        if (e->key + 1 >= types[e->type].model.anims[e->anim].count)
            e->state = ST_DEAD;
        else
            anim_step(e, NULL);
        break;
    default:
        anim_step(e, NULL);
        break;
    }
}

static void spider_shot(Enemy *e)
{
    if (is_spiderling(e)) {
        set_anim(e, SET_GAIT, 3, 0);
        e->state = ST_DYING;
        return;
    }
    e->airborne = 0;
    if (e->y < e->floor_y)
        e->y = e->floor_y;
    if (e->hp < 0) {
        set_anim(e, SET_GAIT, 12, 0);
        e->state = ST_DYING;
        e->sub = 0;
        return;
    }
    if (e->state == SP_CHARGE && e->sub == 3)
        return; // rushing: it keeps on
    set_anim(e, SET_GAIT, 1, 0);
    dog_enter(e, SP_HURT);
    e->sub = 1;
}

// ---------------------------------------------------------------- Mr. X
// The Tyrant (type 0x2a; overlay of CDEMD0.EMS at 0x80100000, states in
// table 0x80104cb4; docs/enemy-others.md). 20000+ HP: shots only slow him
// down. He walks at Leon (turning 0x30 a tick) whenever he is closer than
// 5500 and, in reach and facing him, swipes or punches: three attacks
// (states 25 / 26 / 27: keys, range and damage below). The animation numbers
// are those of the state routines; a spawn with the hold status (0x4000)
// stands until the room script clears it.
#define TYPE_TYRANT 0x2a
#define TY_STAND 0
#define TY_WALK 1
#define TY_ATTACK 3
#define TY_PAUSE 4
#define TY_WALK_ANIM 0
#define TY_STAND_ANIM 1

static const struct { uint8_t anim, key0, key1; uint16_t range; uint8_t damage; } ty_attack[3] = {
    { 12, 0x2F, 0x32, 550, 40 },   // 0x801025b0: the big swing, 231 keys
    { 15, 0x0E, 0x13, 950, 10 },   // 0x80102790: the quick one
    { 3, 0x15, 0x19, 750, 30 },    // 0x801029d4: the backhand
};

static int is_tyrant(const Enemy *e) { return types[e->type].type == TYPE_TYRANT; }

static void tyrant_spawn(Enemy *e)
{
    e->hp = 20000 + (rnd() & 0x7F);
    e->floor_y = e->y;
    set_anim(e, SET_GAIT, TY_STAND_ANIM, 0);
    dog_enter(e, TY_STAND);
}

static void tyrant_tick(Enemy *e, int i, const EnemyTarget *p, uint32_t d)
{
    int32_t fwd;
    int near = d < ((e->flags & 0x300) == 0x100 ? 6500 : 5500);
    switch (e->state) {
    case TY_STAND:
        anim_step(e, NULL);
        if (!(e->flags & EF_HOLD) && near && !p->dead && same_floor(e, p))
            dog_enter(e, TY_WALK);
        break;
    case TY_WALK:
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, TY_WALK_ANIM, e->key);
            e->sub = 1;
            e->timer = 0;
        }
        if (!near || p->dead || (e->flags & EF_HOLD)) {
            dog_enter(e, TY_STAND);
            break;
        }
        e->dir = (e->dir + turn_delta(e, e->has_goal ? e->goal_x : p->x, e->has_goal ? e->goal_z : p->z, 0x30)) & 32767;
        if (d > 900) {
            anim_step(e, &fwd);
            move_dir(e, i, e->dir, fwd > 0 ? fwd : 0);
        } else {
            anim_step(e, NULL);
        }
        if (e->cooldown) {
            e->cooldown--;
        } else if (d < 1500 && p->can_grab && in_cone(e, p->x, p->z, 0x100)) {
            e->aggr = d < 900 ? 0 : d < 1200 ? 1 : 2;           // closer: the big swing
            if (rnd() & 1)
                e->aggr = rnd() % 3;
            dog_enter(e, TY_ATTACK);
        }
        break;
    case TY_ATTACK: {
        const typeof(ty_attack[0]) *a = &ty_attack[e->aggr % 3];
        if (e->sub == 0) {
            set_anim(e, SET_GAIT, a->anim, 0);
            e->sub = 1;
            e->timer2 = 0;
        }
        e->dir = (e->dir + turn_delta(e, p->x, p->z, 8)) & 32767;
        int done = anim_step(e, NULL);
        if (!e->timer2 && e->key >= a->key0 && e->key <= a->key1 && p->can_grab && d < a->range + 600 &&
            in_cone(e, p->x, p->z, 0x180)) {
            game_blood(0, p->x, p->y - 1500, p->z, e->dir, 25, p->y);
            if (game_player_damage(a->damage) == 2)
                game_player_killed();
            else
                game_player_hurt(((p->dir - e->dir + 8192) & 32767) < 16384 ? 2 : 1);
            e->timer2 = 1;
        }
        if (done) {
            e->cooldown = 20;
            dog_enter(e, TY_WALK);
        }
        break;
    }
    default:
        anim_step(e, NULL);
        break;
    }
}

// ---------------------------------------------------------------- pose
// The pose to draw this tick: the animation's keyframe, eased in over
// POSE_BLEND_TICKS after the animation changed (shown = new + (shown - new)
// * left / 8, as FUN_80028a4c does for every character).
static void update_pose(Enemy *e)
{
    const Model *fm = anim_model(e);
    const ModelAnim *a = &fm->anims[e->anim];
    if (!a->count)
        return;
    int key = e->key < a->count ? e->key : a->count - 1;
    int frame = ANIM_FRAME(fm->anim_pool[a->first + key]);
    const ModelFrame *f = model_frame(fm, frame);
    const int16_t *rot = model_frame_rot(fm, frame);
    int bones = types[e->type].model.header->num_bones;
    if (fm->header->num_bones < bones)
        bones = fm->header->num_bones;
    if (bones > POSE_BONES)
        bones = POSE_BONES;
    if (!e->pose_valid || e->no_blend)
        e->pose_blend = 0;
    else if (fm != e->pose_set || e->anim != e->pose_anim)
        e->pose_blend = POSE_BLEND_TICKS;
    e->no_blend = 0;
    e->pose_set = fm;
    e->pose_anim = e->anim;
    e->pose_valid = 1;
    int w = e->pose_blend;
    for (int b = 0; b < bones; b++)
        for (int k = 0; k < 3; k++) {
            int32_t to = rot[b * 3 + k] & 32767;
            int32_t d = ((e->pose_rot[b][k] - to + 16384) & 32767) - 16384;
            e->pose_rot[b][k] = (int16_t)((to + d * w / 8) & 32767);
        }
    for (int k = 0; k < 3; k++)
        e->pose_ofs[k] = (int16_t)(f->offset[k] + (e->pose_ofs[k] - f->offset[k]) * w / 8);
    if (e->pose_blend)
        e->pose_blend--;
}

// ---------------------------------------------------------------- plc
// The character as plc.c sees it: the room's set for mode 0, its own model
// for the rest. Zombies walk with their gait (set 0) and stand with set 1's
// first animation; people (Kendo) walk with set 0's first and stand with set
// 1's third (0x800116bc / 0x800116c8: walk 0, stand 2 of those sets).
static PlcBody enemy_body(const Enemy *e, int slot)
{
    const EnemyType *t = &types[e->type];
    int set0 = t->num_sets > 0 ? t->set_start[0] : 0, set1 = t->num_sets > 1 ? t->set_start[1] : set0;
    PlcBody b = { .x = e->x, .z = e->z, .dir = e->dir, .set = anim_model(e), .anim = e->anim,
                  .key = e->key, .room_set = room_sets[slot], .own_set = &t->model, .own_first = set0,
                  .walk_anim = set0 + (is_person(e) ? 0 : e->gait),
                  .idle_anim = is_person(e) ? set1 + 2 : set1 + A_STAND,
                  .table = t->type >= FIRST_PERSON_TYPE ? t->type - FIRST_PERSON_TYPE : 16 };
    if (b.idle_anim >= t->model.header->num_anims)
        b.idle_anim = 0;
    return b;
}

static void enemy_body_back(Enemy *e, const PlcBody *b)
{
    e->x = b->x;
    e->z = b->z;
    e->dir = b->dir;
    e->anim_set = b->set == &types[e->type].model ? NULL : b->set;
    e->anim = b->anim;
    e->key = b->key;
    e->no_blend |= b->no_blend;
}

void enemy_plc(int slot, const uint8_t *ins)
{
    if (slot < 0 || slot >= MAX_ENEMIES || !enemies[slot].active)
        return;
    Enemy *e = &enemies[slot];
    PlcBody b = enemy_body(e, slot);
    int was = e->plc.active;
    plc_command(&e->plc, &b, ins);
    enemy_body_back(e, &b);
    if (was && !e->plc.active) { // plc_ret: back to its own routine
        e->anim_set = NULL;
        if (is_person(e))
            set_anim(e, SET_MAIN, 2, 0);
        else
            enter_state(e, e->state);
    }
}

static void plc_step(Enemy *e, int slot)
{
    PlcBody b = enemy_body(e, slot);
    plc_tick(&e->plc, &b);
    enemy_body_back(e, &b);
}

// ---------------------------------------------------------------- path zones
// The room's zones (tools/blk2ds.py): rectangles with a neighbour mask.
typedef struct {
    int16_t x0, z0, x1, z1;
    uint16_t unknown, next;
} Zone;
static const Zone *zones;
static int zone_count;

void enemy_set_zones(const void *data)
{
    zones = data ? (const Zone *)((const uint8_t *)data + 4) : NULL;
    zone_count = data ? *(const uint16_t *)data : 0;
}

// FUN_80047260: the last zone holding (x, z), -1 if none
static int zone_at(int32_t x, int32_t z)
{
    for (int n = zone_count - 1; n >= 0; n--) {
        const Zone *b = &zones[n];
        if ((uint16_t)(x - b->x0) < (uint16_t)(b->x1 - b->x0) &&
            (uint16_t)(z - b->z0) < (uint16_t)(b->z1 - b->z0))
            return n;
    }
    return -1;
}

// The middle of the edge zones a and b share, and its ends. across = 1 when
// the edge runs along z (it is crossed in x).
typedef struct {
    int32_t x, z, x1, z1, x2, z2;
    int across;
} Portal;

static Portal portal(int a, int b)
{
    const Zone *p = &zones[a], *q = &zones[b];
    Portal r;
    if (p->z0 == q->z1 || p->z1 == q->z0) {
        r.z = r.z1 = r.z2 = p->z1 == q->z0 ? p->z1 : p->z0;
        r.x1 = p->x0 > q->x0 ? p->x0 : q->x0;
        r.x2 = p->x1 < q->x1 ? p->x1 : q->x1;
        r.x = (r.x1 + r.x2) / 2;
        r.across = 0;
    } else {
        r.x = r.x1 = r.x2 = p->x1 == q->x0 ? q->x0 : p->x0;
        r.z1 = p->z0 > q->z0 ? p->z0 : q->z0;
        r.z2 = p->z1 < q->z1 ? p->z1 : q->z1;
        r.z = (r.z1 + r.z2) / 2;
        r.across = 1;
    }
    return r;
}

static int32_t idist8(int32_t dx, int32_t dz) // sqrt of (dx/8)^2 + (dz/8)^2
{
    dx >>= 3;
    dz >>= 3;
    return (int32_t)sqrtf((float)dx * dx + (float)dz * dz);
}

// FUN_80047308: depth-first over the zone graph from zone a to zone b, every
// simple path; cost = city-block lengths between the edge middles, the last
// two legs as straight lines / 8 (as the PS1 adds them). Heads for the first
// edge of the cheapest path, or straight for the point after it when the
// line to that passes through the edge (its ends pulled in by 800).
static void path_goal(Enemy *e, int a, int b, int32_t tx, int32_t tz)
{
    struct { int zone; uint16_t left; int32_t x, z, cost; Portal first; } st[16];
    int depth = 0, best_cost = 0x7FFFFFFF, found = 0;
    Portal best_first = { 0 };
    int32_t best_next_x = tx, best_next_z = tz;
    st[0].zone = a;
    st[0].left = zones[a].next & ~(1u << a);
    st[0].x = e->x;
    st[0].z = e->z;
    st[0].cost = 0;
    uint16_t visited = 1u << a;
    while (depth >= 0) {
        if (!st[depth].left) {
            visited &= ~(1u << st[depth].zone);
            depth--;
            continue;
        }
        int n = __builtin_ctz(st[depth].left);
        st[depth].left &= ~(1u << n);
        if (n >= zone_count || (visited & (1u << n)))
            continue;
        Portal pt = portal(st[depth].zone, n);
        if (n == b) {
            int32_t cost = st[depth].cost + idist8(st[depth].x - pt.x, st[depth].z - pt.z) +
                           idist8(tx - pt.x, tz - pt.z);
            if (cost < best_cost) {
                best_cost = cost;
                found = 1;
                best_first = depth ? st[1].first : pt;
                best_next_x = depth == 0 ? tx : depth == 1 ? pt.x : st[2].x;
                best_next_z = depth == 0 ? tz : depth == 1 ? pt.z : st[2].z;
            }
            continue;
        }
        if (depth + 1 >= 16)
            continue;
        depth++;
        st[depth].zone = n;
        st[depth].left = zones[n].next & ~visited;
        st[depth].x = pt.x;
        st[depth].z = pt.z;
        st[depth].first = depth == 1 ? pt : st[depth - 1].first;
        st[depth].cost = st[depth - 1].cost + abs(st[depth - 1].x - pt.x) + abs(st[depth - 1].z - pt.z);
        visited |= 1u << n;
    }
    if (!found)
        return;
    // approaching an edge at a flat angle: first a point 800 short of it
    Portal f = best_first;
    int32_t gx = f.x, gz = f.z, sx = 0, sz = 0;
    if (!f.across) {
        int32_t dz = gz - e->z;
        if (dz == 0 || abs((gx - e->x) * 4096 / dz) > 10000)
            gz += dz < 0 ? 800 : -800;
        sx = 800;
    } else {
        int32_t dx = gx - e->x;
        if (dx == 0 || abs((gz - e->z) * 4096 / dx) > 10000)
            gx += dx < 0 ? 800 : -800;
        sz = 800;
    }
    // the line to the point after the edge passes between its ends: go there
    int32_t vx = best_next_x - e->x, vz = best_next_z - e->z;
    int64_t c1 = (int64_t)(f.z1 - e->z + sz) * vx - (int64_t)(f.x1 - e->x + sx) * vz;
    int64_t c2 = (int64_t)(f.z2 - e->z - sz) * vx - (int64_t)(f.x2 - e->x - sx) * vz;
    if ((c1 < 0) != (c2 < 0)) {
        gx = best_next_x;
        gz = best_next_z;
    }
    e->goal_x = gx;
    e->goal_z = gz;
    e->has_goal = 1;
}

// FUN_80046fb0 (every 8 frames): Leon himself if the zombie may go straight
// (status 0x1000, no zones, same zone), else the next edge on the way.
static void update_goal(Enemy *e, const EnemyTarget *p)
{
    if ((e->flags & 0x1000) || !zone_count) {
        e->has_goal = 0;
        return;
    }
    if (e->flags & EF_HOLD)
        return;
    int a = zone_at(e->x, e->z), b = zone_at(p->x, p->z);
    if (a < 0 || b < 0)
        return; // keeps the last goal
    if (a == b) {
        e->has_goal = 0;
        return;
    }
    path_goal(e, a, b, p->x, p->z);
}

void enemy_tick(const EnemyTarget *p)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!present(e) || !anim_model(e)->anims[e->anim].count)
            continue;
        float fdx = (float)(p->x - e->x), fdz = (float)(p->z - e->z);
        uint32_t d = (uint32_t)sqrtf(fdx * fdx + fdz * fdz);
        if (e->moan_pause)
            e->moan_pause--;
        if (e->sight_timer-- == 0) {
            e->sight_timer = 7;
            e->sees = !game_sight_blocked(e->x, e->y + EYE_ENEMY, e->z, p->x, p->y + EYE_PLAYER, p->z);
            update_goal(e, p);
        }
        if (e->push > 0) // thrown back / heavy recoil (0x80109550, 0x80105b00)
            push_tick(e, i);
        if (e->bleed) {
            if (!(e->bleed & 1))
                game_blood(0, e->x, e->y - 1800 - (rnd() & 0x1FF), e->z, e->push_dir + (rnd() & 0xFFF) - 0x800,
                           10 + (rnd() & 15), e->y);
            e->bleed--;
        }
        if (e->state == ST_DEAD)
            continue;
        if (e->plc.active) {
            plc_step(e, i);
            continue;
        }
        if (e->victim) { // falls, then stays down
            const ModelAnim *a = &anim_model(e)->anims[e->anim];
            if (e->key + 1 < a->count)
                e->key++;
            continue;
        }
        if (is_person(e)) { // stands (routine 1 of the people is not ported)
            anim_step(e, NULL);
            continue;
        }
        if (e->routine == 8) {
            script_mode(e, i);
            continue;
        }
        if (is_dog(e)) {
            dog_tick(e, i, p, d);
            continue;
        }
        if (is_crow(e)) {
            crow_tick(e, i, p, d);
            continue;
        }
        if (is_licker(e)) {
            licker_tick(e, i, p, d);
            continue;
        }
        if (is_spider(e)) {
            spider_tick(e, i, p, d);
            continue;
        }
        if (is_tyrant(e)) {
            tyrant_tick(e, i, p, d);
            continue;
        }
        if (e->flinch) { // hit recoil: holds still a moment
            e->flinch--;
            anim_step(e, NULL);
            continue;
        }
        switch (e->state) {
        case ST_DYING:
        case ST_DOWN: fallen(e, i); break;
        case ST_LURK: lurk(e, i, p->x, p->z, d, p->noise); break;
        case ST_CHASE: chase(e, i, p, d); break;
        case ST_LUNGE: lunge(e, i, p, d); break;
        case ST_GRAB: grab_zombie(e, i, p->pressed); break;
        case ST_LYING: lying(e); break;
        default: anim_step(e, NULL); break;
        }
    }
    if (grab.enemy >= 0 && enemies[grab.enemy].state == ST_GRAB)
        grab_player(&enemies[grab.enemy]);
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (present(&enemies[i]))
            update_pose(&enemies[i]);
}

int enemy_grabbing(void) { return grab.enemy >= 0; }

static int alive(const Enemy *e) { return present(e) && e->state < ST_DYING && !is_person(e); }

// A zombie on the ground (status obj+0x10e bit 0x2000: set when it is
// knocked down or spawns lying, cleared when it gets up) is a target only
// if no standing one is in sight.
static int on_ground(const Enemy *e) { return e->state == ST_DOWN || e->state == ST_LYING; }

// Nearest living enemy Leon can see within range (0 = any), for the
// auto-aim when he raises a weapon (FUN_8003ed74): standing ones first, those
// on the ground only if there is none. -1 if none.
int enemy_aim_target(int32_t x, int32_t y, int32_t z, int range, int32_t *tx, int32_t *tz)
{
    int best = -1, low = -1;
    int64_t best_d = 0, low_d = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!alive(e))
            continue;
        int64_t dx = e->x - x, dz = e->z - z, d = dx * dx + dz * dz;
        if (range && d > (int64_t)range * range)
            continue;
        if (game_sight_blocked(x, y + EYE_PLAYER, z, e->x, e->y + EYE_ENEMY, e->z))
            continue;
        if (on_ground(e)) {
            if (low < 0 || d < low_d) {
                low = i;
                low_d = d;
            }
        } else if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (best < 0)
        best = low;
    if (best >= 0) {
        *tx = enemies[best].x;
        *tz = enemies[best].z;
    }
    return best;
}

static void fall(Enemy *e, int dying)
{
    e->fall_back = rnd() & 1;
    e->routine = 1; // shot down: leaves the room script's control
    e->state = dying ? ST_DYING : ST_DOWN;
    e->sub = 1;
    e->standing = 0;
    e->flinch = 0;
    e->timer = rnd() / 2 + 60; // knocked down: lies this long
    set_anim(e, SET_MAIN, A_FALL + e->fall_back, 0);
    if (grab.enemy == (int)(e - enemies))
        grab.enemy = -1;
}

// the shotgun's impulse: pushed along push_dir, 50 less every tick
static void push_tick(Enemy *e, int i)
{
    int32_t mx = (e->push * cosLerp(e->push_dir)) >> 12, mz = -((e->push * sinLerp(e->push_dir)) >> 12);
    if (!game_blocked(e->x, e->z, e->x + mx, e->z, e->y) && !crowded(i, e->x + mx, e->z))
        e->x += mx;
    if (!game_blocked(e->x, e->z, e->x, e->z + mz, e->y) && !crowded(i, e->x, e->z + mz))
        e->z += mz;
    e->push -= 50;
}

// a burst of blood where the shell hits (0x80109550: two big ones, then 1-4
// spatters fanned around the shot direction)
static void blood_burst(const Enemy *e, int16_t dir)
{
    int n = 3 + (rnd() & 3);
    for (int k = 0; k < n; k++)
        game_blood(0, e->x, e->y - 1900 - (rnd() & 0x3FF), e->z, dir + (rnd() & 0x1FFF) - 0x1000,
                   20 + (rnd() & 31), e->y);
}

// A shot or stab: the nearest living enemy within +-22.5 degrees of dir that
// Leon can see, within range (0 = any). damage[3] by distance (near < 3000,
// < 6000, farther; table 0x8009c470). Returns the enemy hit or -1.
int enemy_hit(int32_t x, int32_t y, int32_t z, int16_t dir, int range, int weapon, const uint16_t damage[3], int low)
{
    int best = -1;
    int64_t best_d = 0;
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!alive(e))
            continue;
        if (!low && (e->state == ST_DOWN || e->state == ST_LYING))
            continue; // on the ground: the shot passes over it
        int64_t dx = e->x - x, dz = e->z - z, d = dx * dx + dz * dz;
        if (range && d > (int64_t)range * range)
            continue;
        int a = (int)(atan2f((float)-dz, (float)dx) * 32768.0f / (2.0f * 3.14159265f)) & 32767;
        int diff = ((a - dir + 16384) & 32767) - 16384;
        if (abs(diff) > 0x100 * 8)
            continue;
        if (game_sight_blocked(x, y + EYE_PLAYER, z, e->x, e->y + EYE_ENEMY, e->z))
            continue;
        if (best < 0 || d < best_d) {
            best = i;
            best_d = d;
        }
    }
    if (best < 0)
        return -1;
    Enemy *e = &enemies[best];
    float dist = sqrtf((float)best_d);
    int box = dist < 3000 ? 0 : dist < 6000 ? 1 : 2;
    if (is_crow(e)) { // 10 HP, every weapon does 15 or more
        e->hp -= damage[box];
        game_blood(0, e->x, e->y - 200, e->z, dir, 15, e->floor_y);
        crow_shot(e, best, dir);
        return best;
    }
    if (is_tyrant(e)) { // he shrugs it off
        e->hp -= damage[box];
        game_blood(0, e->x, e->y - 1800, e->z, dir, 20, e->floor_y);
        return best;
    }
    if (is_spider(e)) {
        e->hp -= damage[box];
        game_blood(1, e->x, e->y - 400, e->z, dir, 20, e->floor_y);
        spider_shot(e);
        return best;
    }
    if (is_licker(e)) {
        const uint16_t *ld = lick_damage[lick_kind(e)][weapon > 0 && weapon < 20 ? weapon : 0];
        e->hp -= ld[0] ? ld[box] : damage[box];
        game_blood(0, e->x, e->y - 900, e->z, dir, 20, e->floor_y);
        licker_shot(e, best, dir);
        return best;
    }
    if (is_dog(e)) {
        const uint16_t *dd = weapon > 0 && weapon < 20 && dog_damage[weapon][0] ? dog_damage[weapon] : damage;
        e->hp -= dd[box];
        game_blood(0, e->x, e->y - 700, e->z, dir, 20, e->floor_y);
        if (weapon >= 5 && weapon <= 8 && box < 2)
            game_blood(0, e->x, e->y - 600, e->z, dir + 0x800, 30, e->floor_y);
        dog_shot(e, dir, e->hp < 0);
        return best;
    }
    e->hp -= damage[box];
    game_blood(0, e->x, e->y - 2300, e->z, dir, 20, e->y);
    // The shotgun (zombie tables 0x8010c764 / 0x8010ca48, weapons 7 and 8,
    // upper body): near it throws the zombie over (0x8010663c; the PS1 then
    // often uses "thrown backward" 0x80109550 - impulse 400, -50 per tick,
    // a burst of blood, sound 0xb / 0xd - and crawlers / head bursts, which
    // need parts this port does not have yet: thrown backward always).
    // Middle distance: the heavy recoil 0x80105b00 (impulse 450, bleeding for
    // 10 ticks, sound 0xc / 0xd) when it survives; far: the normal hit.
    // The magnums kill with every hit; the PS1 bursts the head then
    // (0x80109204, part 8 flies off, which this port does not have yet):
    // the burst of blood and the fall
    if ((weapon == 5 || weapon == 6) && !is_person(e) && e->hp < 0) {
        blood_burst(e, dir);
        fall(e, 1);
        sound_at(e, (rnd() & 1) ? SND_MOAN + 3 : SND_MOAN + 1);
        return best;
    }
    if ((weapon == 7 || weapon == 8) && !is_person(e) && box < 2) {
        e->push_dir = dir;
        if (box == 0) {
            blood_burst(e, dir);
            e->push = 400;
            fall(e, e->hp < 0);
            sound_at(e, (rnd() & 1) ? SND_MOAN + 3 : SND_MOAN + 1);
            return best;
        }
        if (e->hp >= 0 && e->state != ST_LYING && e->state != ST_DOWN && e->state != ST_GRAB) {
            e->push = 450;
            e->bleed = 10;
            e->flinch = 15;
            if (!e->moan_pause) {
                sound_at(e, (rnd() & 1) ? SND_MOAN + 2 : SND_MOAN + 3);
                e->moan_pause = MOAN_PAUSE;
            }
            if (e->state == ST_LURK)
                enter_state(e, ST_CHASE);
            return best;
        }
    }
    if (e->hp < 0) { // dies (0x80108470)
        fall(e, 1);
        sound_at(e, (rnd() & 1) ? SND_MOAN + 3 : SND_MOAN + 1);
        return best;
    }
    if (e->state == ST_LYING || e->state == ST_DOWN || e->state == ST_GRAB)
        return best;
    // the hit (0x80105378): a short recoil, a groan; twice out of stagger: falls
    e->flinch = 6;
    if (!e->moan_pause) {
        sound_at(e, (rnd() & 1) ? SND_MOAN + 1 : SND_MOAN + 2);
        e->moan_pause = MOAN_PAUSE;
    }
    if (weapon > 0 && weapon < 20)
        e->stagger -= stagger_hit[weapon];
    if (e->stagger <= 0) {
        if (e->stagger_out || weapon == 1) {
            fall(e, 0);
            e->stagger_out = 0;
        } else {
            e->stagger_out = 1;
        }
        e->stagger = (rnd() & 0xF) + 0x10;
    }
    if (e->state == ST_LURK)
        enter_state(e, ST_CHASE); // shot at: comes for Leon
    return best;
}

// falling (dead or knocked down), lying, getting up
static void fallen(Enemy *e, int i)
{
    int32_t fwd;
    if (e->sub == 1) { // falls with the animation's motion
        int done = anim_step(e, &fwd);
        walk(e, i, fwd);
        if (done) {
            if (e->state == ST_DYING) {
                e->state = ST_DEAD;
                return;
            }
            e->sub = 2;
        }
        return;
    }
    if (e->state != ST_DOWN)
        return;
    if (e->sub == 2) {
        if (e->timer-- > 0)
            return;
        set_anim(e, SET_MAIN, e->fall_back ? A_GET_UP + 1 : A_GET_UP, 0);
        e->sub = 3;
        moan(e, SND_MOAN + 2);
    } else if (anim_step(e, NULL)) {
        enter_state(e, ST_CHASE);
    }
}

static void draw_bone(const EnemyType *t, const Enemy *e, int bone)
{
    const Model *m = &t->model;
    const ModelBone *b = &m->bones[bone];
    const int16_t *rot = e->pose_rot[bone < POSE_BONES ? bone : 0];
    glPushMatrix();
    if (b->parent < 0)
        glTranslatef32(e->pose_ofs[0], e->pose_ofs[1], e->pose_ofs[2]);
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
        draw_bone(t, e, c);
    glPopMatrix(1);
}

// Is the point roughly inside the current view? (camera from main.c)
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t margin);
// main.c: seen by the camera only through a wall (room boxes, not masks)
int camera_occluded(int32_t x, int32_t y, int32_t z);
extern int cull_models;  // main.c: POLY_CULL_* that drops faces turned away

void enemy_draw(int reserve_vertices, int reserve_polygons)
{
    // nearest first: simple selection on the distance to the camera
    int order[MAX_ENEMIES], n = 0;
    int32_t dist[MAX_ENEMIES];
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!present(e) || !types[e->type].drawable || !anim_model(e)->anims[e->anim].count)
            continue;
        int32_t d = camera_sees(e->x, e->y - 900, e->z, 1200);
        if (d >= 0 && camera_occluded(e->x, e->y, e->z))
            d = -1;
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
        if (!e->pose_valid)
            update_pose(e);
        glPolyFmt(POLY_ALPHA(31) | cull_models | POLY_ID(3 + order[k]));
        glPushMatrix();
        glTranslatef32(e->x, e->y, e->z);
        glRotateYi(e->dir);
        if (e->flinch) // the shot pushes the upper body back
            glRotateZi(-e->flinch * 220);
        for (int b = 0; b < t->model.header->num_bones; b++)
            if (t->model.bones[b].parent < 0)
                draw_bone(t, e, b);
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
