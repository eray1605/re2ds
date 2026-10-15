// re2ds - effect sprites (see effect.h).
#include <nds.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "effect.h"

#define EFFECT_MAGIC 0x31534552 // "RES1"
#define MAX_SPRITES 32
#define GRAVITY 6               // world units per tick per tick (+Y is down)
#define WORLD_PER_TEXEL 20      // sprite size in the world
#define TOWARD_CAMERA 500       // drawn this much nearer, so the body it comes from does not hide it

typedef struct {
    uint8_t effect, sub;
    uint16_t first, count, pad;
} EffectAnim;

typedef struct {
    uint8_t u, v, side;
    int8_t ox, oy;
    uint8_t time;
    uint16_t pad;
} EffectFrame;

typedef struct {
    uint8_t active;
    uint8_t anim;
    uint16_t frame, timer;
    int32_t x, y, z, vx, vy, vz, floor;
} Sprite;

static uint8_t *data;
static const EffectAnim *anims;
static const EffectFrame *frames;
static int num_anims;
static int tex = -1;
static Sprite sprites[MAX_SPRITES];
int effect_status, effect_spawned; // diagnostics: 1 loaded, 2 texture ok

void effect_load(void)
{
    uint32_t size;
    data = asset_load("nitro:/effect/core.bin", &size);
    if (!data || *(uint32_t *)data != EFFECT_MAGIC) {
        asset_free(data);
        data = NULL;
        return;
    }
    num_anims = *(uint16_t *)(data + 4);
    int num_frames = *(uint16_t *)(data + 6);
    anims = (const EffectAnim *)(data + 8);
    frames = (const EffectFrame *)(anims + num_anims);
    const uint16_t *palette = (const uint16_t *)(frames + num_frames);
    const uint8_t *texels = (const uint8_t *)(palette + 16);
    DC_FlushRange(data, size);
    glGenTextures(1, &tex);
    glBindTexture(0, tex);
    effect_status = 1;
    if (glTexImage2D(0, 0, GL_RGB16, TEXTURE_SIZE_256, TEXTURE_SIZE_256, 0,
                     TEXGEN_TEXCOORD | GL_TEXTURE_COLOR0_TRANSPARENT, texels))
        effect_status = 2;
    glColorTableEXT(0, 0, 16, 0, 0, palette);
}

void effect_clear(void)
{
    memset(sprites, 0, sizeof(sprites));
}

static int find_anim(int effect, int sub)
{
    for (int i = 0; i < num_anims; i++)
        if (anims[i].effect == effect && anims[i].sub == sub)
            return i;
    return -1;
}

void effect_blood(int kind, int32_t x, int32_t y, int32_t z, int16_t dir, int speed, int32_t floor_y)
{
    int a = data ? find_anim(0, kind) : -1;
    if (a < 0)
        return;
    for (int i = 0; i < MAX_SPRITES; i++) {
        Sprite *s = &sprites[i];
        if (s->active)
            continue;
        effect_spawned++;
        s->active = 1;
        s->anim = a;
        s->frame = 0;
        s->timer = frames[anims[a].first].time;
        s->x = x;
        s->y = y;
        s->z = z;
        s->vx = (speed * cosLerp(dir)) >> 12;
        s->vz = -((speed * sinLerp(dir)) >> 12);
        s->vy = -speed / 2;
        s->floor = floor_y;
        return;
    }
}

void effect_tick(void)
{
    for (int i = 0; i < MAX_SPRITES; i++) {
        Sprite *s = &sprites[i];
        if (!s->active)
            continue;
        s->x += s->vx;
        s->z += s->vz;
        s->y += s->vy;
        s->vy += GRAVITY;
        if (s->y >= s->floor) { // lands
            s->y = s->floor;
            s->vx = s->vz = s->vy = 0;
        }
        const EffectAnim *a = &anims[s->anim];
        if (s->timer > 1 && s->timer != 255) {
            s->timer--;
            continue;
        }
        if (++s->frame >= a->count) {
            s->active = 0;
            continue;
        }
        s->timer = frames[a->first + s->frame].time;
        if (s->timer == 255) // the PS1 holds this frame; here a short pause
            s->timer = 4;
    }
}

int effect_count(void)
{
    int n = 0;
    for (int i = 0; i < MAX_SPRITES; i++)
        n += sprites[i].active;
    return n;
}

void effect_draw(const int32_t cam_from[3], const int32_t cam_to[3])
{
    if (!data || !effect_count())
        return;
    // camera right and up (up = -Y in the room) for the billboards
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
    u[0] = r[1] * f[2] - r[2] * f[1];
    u[1] = r[2] * f[0] - r[0] * f[2];
    u[2] = r[0] * f[1] - r[1] * f[0];
    len = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int k = 0; k < 3; k++)
        u[k] /= len;
    if (u[1] > 0)
        for (int k = 0; k < 3; k++)
            u[k] = -u[k];

    glBindTexture(0, tex);
    glPolyFmt(POLY_ALPHA(26) | POLY_CULL_NONE | POLY_ID(62));
    for (int i = 0; i < MAX_SPRITES; i++) {
        const Sprite *s = &sprites[i];
        if (!s->active)
            continue;
        const EffectFrame *fr = &frames[anims[s->anim].first + s->frame];
        float h = fr->side * WORLD_PER_TEXEL * 0.5f;
        int16_t rx = (int16_t)(r[0] * h), rz = (int16_t)(r[2] * h);
        int16_t ux = (int16_t)(u[0] * h), uy = (int16_t)(u[1] * h), uz = (int16_t)(u[2] * h);
        glPushMatrix();
        glTranslatef32(s->x + nearer[0], s->y + nearer[1], s->z + nearer[2]);
        glBegin(GL_QUADS);
        glTexCoord2t16(inttot16(fr->u), inttot16(fr->v));
        glVertex3v16(-rx + ux, uy, -rz + uz);
        glTexCoord2t16(inttot16(fr->u + fr->side), inttot16(fr->v));
        glVertex3v16(rx + ux, uy, rz + uz);
        glTexCoord2t16(inttot16(fr->u + fr->side), inttot16(fr->v + fr->side));
        glVertex3v16(rx - ux, -uy, rz - uz);
        glTexCoord2t16(inttot16(fr->u), inttot16(fr->v + fr->side));
        glVertex3v16(-rx - ux, -uy, -rz - uz);
        glEnd();
        glPopMatrix(1);
    }
}
