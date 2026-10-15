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

// The DS geometry engine takes at most 6144 vertices (and 2048 polygons) per
// frame and silently drops the rest, so enemies would flicker. Enemies are
// drawn nearest first while they fit; Leon and the masks are counted first.
#define VERTEX_LIMIT 6144

// collision with the room, shared with the player (main.c)
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y);

typedef struct {
    int type;                   // 0 = free
    Model model;
    uint8_t *tex_data;
    int tex[MAX_PAGES];
    int pages;
    int drawable;               // textures uploaded
    int vertices;               // vertices one instance sends to the GPU
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

// diagnostics for the debug screen
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

// Vertices in all display lists of a model (packed GX: count VTX_16 commands).
int model_vertex_count(const Model *m)
{
    int total = 0, lists = m->header->num_bones * m->header->num_pages;
    for (int i = 0; i < lists; i++) {
        if (!m->lists[i])
            continue;
        const uint32_t *w = (const uint32_t *)((const uint8_t *)m->data + m->lists[i]);
        uint32_t n = *w++, k = 0;
        while (k < n) {
            uint32_t ids = w[k++];
            for (int c = 0; c < 4; c++) {
                int cmd = (ids >> (8 * c)) & 0xFF;
                if (cmd == 0x23) {
                    total++;
                    k += 2;
                } else if (cmd == 0x20 || cmd == 0x22 || cmd == 0x40) {
                    k += 1;
                }
            }
        }
    }
    return total;
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

    t->vertices = model_vertex_count(&t->model);

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

void enemy_spawn(int slot, int type, int variant, int32_t x, int32_t y, int32_t z, int16_t dir_re2)
{
    (void)variant;
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
    e->dir = dir_re2 * 8;
    int anims = types[t].model.header->num_anims;
    if (anims == 0) {
        e->active = 0;
        enemy_last_error = 4;
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
        if (!game_blocked(e->x, e->z, e->x + mx, e->z, e->y) && !crowded(i, e->x + mx, e->z))
            e->x += mx;
        if (!game_blocked(e->x, e->z, e->x, e->z + mz, e->y) && !crowded(i, e->x, e->z + mz))
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

// Is the point roughly inside the current view? (camera from main.c)
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t margin);

void enemy_draw(int vertices_used)
{
    // nearest first: simple selection on the distance to the camera
    int order[MAX_ENEMIES], n = 0;
    int32_t dist[MAX_ENEMIES];
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (!e->active || !types[e->type].drawable || !types[e->type].model.anims[e->anim].count)
            continue;
        int32_t d = camera_sees(e->x, e->y - 900, e->z, 1200);
        if (d < 0)
            continue; // off screen
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
        const Enemy *e = &enemies[order[k]];
        const EnemyType *t = &types[e->type];
        if (vertices_used + t->vertices > VERTEX_LIMIT)
            break;
        vertices_used += t->vertices;
        const ModelAnim *a = &t->model.anims[e->anim];
        int frame = ANIM_FRAME(t->model.anim_pool[a->first + e->key]);
        glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(3 + order[k]));
        glPushMatrix();
        glTranslatef32(e->x, e->y, e->z);
        glRotateYi(e->dir);
        for (int b = 0; b < t->model.header->num_bones; b++)
            if (t->model.bones[b].parent < 0)
                draw_bone(t, b, frame);
        glPopMatrix(1);
    }
}

#ifndef ARM9
// host tests: positions and step per tick
void enemy_debug_print(int tick)
{
    for (int i = 0; i < MAX_ENEMIES; i++) {
        const Enemy *e = &enemies[i];
        if (e->active)
            printf("t%d e%d anim %d key %d pos %d %d dir %d verts %d\n", tick, i, e->anim, e->key,
                   (int)e->x, (int)e->z, e->dir, types[e->type].vertices);
    }
}
#endif
