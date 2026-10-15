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
#define CHASE_DISTANCE 20000    // farther away: stand still
#define TURN_NEAR 16            // RE2 units per game tick when d < 3000
#define TURN_FAR 8              // otherwise (pl. 0x801019d0)
#define GAIT_COUNT 8            // set 0: one of 8 gaits, chosen at random

// collision with the room, shared with the player (main.c)
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y);

typedef struct {
    int type;                   // 0 = free
    Model model;
    uint8_t *tex_data;
    int tex[MAX_PAGES];
    int pages;
    int drawable;               // textures uploaded
    int8_t first_child[64], next_sibling[64];
} EnemyType;

typedef struct {
    uint8_t active;
    int8_t type;                // index into types[]
    int32_t x, y, z;
    int16_t dir;                // DS units
    int anim, key;
} Enemy;

static EnemyType types[MAX_TYPES];
static Enemy enemies[MAX_ENEMIES];

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

static int load_type(int type)
{
    for (int i = 0; i < MAX_TYPES; i++)
        if (types[i].type == type)
            return i;
    int slot = -1;
    for (int i = 0; i < MAX_TYPES && slot < 0; i++)
        if (!types[i].type)
            slot = i;
    if (slot < 0)
        return -1;
    EnemyType *t = &types[slot];
    char path[48];
    snprintf(path, sizeof(path), "nitro:/model/em%02x.bin", type);
    if (!model_load(&t->model, path)) {
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
                              page + 512))
                t->drawable = 0; // texture memory full
            glColorTableEXT(0, 0, 256, 0, 0, (const uint16_t *)page);
        }
    }
    return slot;
}

void enemy_spawn(int slot, int type, int variant, int32_t x, int32_t y, int32_t z, int16_t dir_re2)
{
    (void)variant;
    if (slot < 0 || slot >= MAX_ENEMIES)
        return;
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
    e->dir = dir_re2 * 8;
    int anims = types[t].model.header->num_anims;
    if (anims == 0) {
        e->active = 0;
        return;
    }
    e->anim = anims >= GAIT_COUNT ? rand() % GAIT_COUNT : 0;
    int count = types[t].model.anims[e->anim].count;
    e->key = count ? rand() % count : 0;
}

int enemy_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ENEMIES; i++)
        n += enemies[i].active;
    return n;
}

static int32_t frame_speed(const Model *m, const ModelAnim *a, int key)
{
    return model_frame(m, ANIM_FRAME(m->anim_pool[a->first + key]))->speed[0];
}

void enemy_tick(int32_t player_x, int32_t player_z)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        Enemy *e = &enemies[i];
        if (!e->active)
            continue;
        const Model *m = &types[e->type].model;
        const ModelAnim *a = &m->anims[e->anim];
        if (!a->count)
            continue;
        int32_t dx = player_x - e->x, dz = player_z - e->z;
        float d = sqrtf((float)dx * dx + (float)dz * dz);
        if (d > CHASE_DISTANCE)
            continue;

        // turn toward the player; facing = (cos dir, -sin dir)
        int16_t want = (int16_t)(atan2f((float)-dz, (float)dx) * 32768.0f / (2.0f * 3.14159265f));
        int16_t diff = want - e->dir;
        int step = (d < 3000 ? TURN_NEAR : TURN_FAR) * 8;
        if (diff > step)
            e->dir += step;
        else if (diff < -step)
            e->dir -= step;
        else
            e->dir = want;

        // next keyframe and its root motion
        int prev = e->key;
        e->key = (e->key + 1) % a->count;
        int32_t s0 = e->key == 0 ? 0 : frame_speed(m, a, prev);
        int32_t step_len = frame_speed(m, a, e->key) - s0;
        if (step_len < 0)
            step_len = frame_speed(m, a, e->key);
        if (d < 700)
            step_len = 0; // close enough: no attacks yet, just stand
        int32_t mx = (step_len * cosLerp(e->dir)) >> 12;
        int32_t mz = -((step_len * sinLerp(e->dir)) >> 12);
        if (!game_blocked(e->x, e->z, e->x + mx, e->z, e->y))
            e->x += mx;
        if (!game_blocked(e->x, e->z, e->x, e->z + mz, e->y))
            e->z += mz;
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

void enemy_draw(void)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!e->active || !types[e->type].drawable)
            continue;
        const EnemyType *t = &types[e->type];
        const ModelAnim *a = &t->model.anims[e->anim];
        if (!a->count)
            continue;
        int frame = ANIM_FRAME(t->model.anim_pool[a->first + e->key]);
        glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(3 + i));
        glPushMatrix();
        glTranslatef32(e->x, e->y, e->z);
        glRotateYi(e->dir);
        for (int b = 0; b < t->model.header->num_bones; b++)
            if (t->model.bones[b].parent < 0)
                draw_bone(t, b, frame);
        glPopMatrix(1);
    }
}
