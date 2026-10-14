// re2ds - Resident Evil 2 room prototype for the Nintendo DS.
//
// Main screen: pre-rendered background on BG3 (16-bit bitmap) with the 3D layer
// (BG0) on top: Leon plus the foreground masks as depth-tested quads.
// Sub screen: debug text.
//
// Conventions (verified with tools/ds_preview.py, see formats.h):
//   world units = RE2 game units = DS 20.12 fixed point, -Y is up,
//   camera = lookAt(from, to, up = -Y), models face +X, facing = (cos a, 0, -sin a).
#include <nds.h>
#include <filesystem.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"

#define ROOM_NAME "room100"
#define RDT_NAME "room1000"

// Animation numbers in leon.bin: 10 PLD animations, then the PL00W00 set.
enum { ANIM_WALK = 10, ANIM_RUN = 11, ANIM_IDLE = 12 };

// Turn speeds per 60 Hz frame in DS angle units (RE2 values per 30 Hz frame in
// 4096 units, see re2ds-notes/player-control.md: x8 for 32768 units, /2 for 60 Hz).
enum { TURN_IDLE = 320, TURN_WALK = 160, TURN_RUN = 224 };

#define PLAYER_RADIUS 400

typedef enum { ST_IDLE, ST_WALK, ST_RUN, ST_BACK } PlayerState;

static Room room;
static Model leon;
static Masks masks;
static int have_masks;
static int leon_tex[3];
static int mask_tex = -1;
static int bg_id;
static int cur_cam = -1;
static int tex_ok, mask_ok;   // glTexImage2D results, shown on the debug screen
static int8_t first_child[64], next_sibling[64];

static struct {
    int32_t x, y, z;
    int16_t dir;
    PlayerState state;
    int anim, key, tick;
} player;

// ---------------------------------------------------------------- loading
static void fatal(const char *msg)
{
    consoleDemoInit();
    printf("\x1b[1;1HERROR:\n%s\n", msg);
    while (1)
        swiWaitForVBlank();
}

static void upload_leon_textures(void)
{
    uint32_t size;
    uint8_t *tex = asset_load("nitro:/model/leon_tex.bin", &size);
    if (!tex)
        fatal("model/leon_tex.bin missing");
    // Texture and palette uploads use DMA, which reads RAM, not the CPU cache.
    DC_FlushRange(tex, size);
    int pages = leon.header->num_pages;
    glGenTextures(pages, leon_tex);
    for (int p = 0; p < pages; p++) {
        const uint8_t *page = tex + p * (512 + TEX_PAGE_WIDTH * TEX_PAGE_HEIGHT);
        glBindTexture(0, leon_tex[p]);
        tex_ok += glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_256, 0,
                               TEXGEN_TEXCOORD, page + 512);
        glColorTableEXT(0, 0, 256, 0, 0, (const uint16_t *)page);
    }
    asset_free(tex);
}

static int tex_size_enum(int n)
{
    switch (n) {
    case 8: return TEXTURE_SIZE_8;
    case 16: return TEXTURE_SIZE_16;
    case 32: return TEXTURE_SIZE_32;
    case 64: return TEXTURE_SIZE_64;
    case 128: return TEXTURE_SIZE_128;
    case 256: return TEXTURE_SIZE_256;
    case 512: return TEXTURE_SIZE_512;
    default: return TEXTURE_SIZE_1024;
    }
}

static void set_camera(int cam)
{
    char path[64];
    cur_cam = cam;

    // background bitmap straight into BG3
    snprintf(path, sizeof(path), "nitro:/bg/" ROOM_NAME "_cam%02d.bin", cam);
    uint16_t *bg = asset_load(path, NULL);
    if (!bg)
        fatal(path);
    DC_FlushRange(bg, BG_WIDTH * BG_HEIGHT * 2);
    dmaCopy(bg, bgGetGfxPtr(bg_id), BG_WIDTH * BG_HEIGHT * 2);
    asset_free(bg);

    // masks for this camera (optional)
    if (have_masks)
        asset_free(masks.data);
    if (mask_tex >= 0) {
        glDeleteTextures(1, &mask_tex);
        mask_tex = -1;
    }
    snprintf(path, sizeof(path), "nitro:/mask/" ROOM_NAME "_cam%02d.bin", cam);
    have_masks = masks_load(&masks, path);
    mask_ok = 0;
    if (have_masks) {
        DC_FlushRange(masks.data, 512 + sizeof(MaskHeader) + 256 * masks.header->tex_height);
        glGenTextures(1, &mask_tex);
        glBindTexture(0, mask_tex);
        mask_ok = glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_256, tex_size_enum(masks.header->tex_height), 0,
                               TEXGEN_TEXCOORD | GL_TEXTURE_COLOR0_TRANSPARENT, masks.pixels);
        glColorTableEXT(0, 0, 256, 0, 0, masks.palette);
    }
}

static void build_hierarchy(void)
{
    int n = leon.header->num_bones;
    memset(first_child, -1, sizeof(first_child));
    memset(next_sibling, -1, sizeof(next_sibling));
    for (int b = n - 1; b >= 0; b--) {
        int parent = leon.bones[b].parent;
        if (parent >= 0) {
            next_sibling[b] = first_child[parent];
            first_child[parent] = b;
        }
    }
}

// ---------------------------------------------------------------- rendering
static void setup_projection(const RoomCamera *c)
{
    // PS1 projects onto 320x240 with screen distance c->screen; same vertical
    // field of view on 256x192.
    float fovy = 2.0f * atanf(120.0f / (float)c->screen) * 180.0f / 3.14159265f;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fovy, 256.0f / 192.0f, 0.05f, 16.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    gluLookAtf32(c->from[0], c->from[1], c->from[2],
                 c->to[0], c->to[1], c->to[2],
                 0, -inttof32(1), 0);
}

static void draw_bone(int bone, int frame)
{
    const ModelBone *b = &leon.bones[bone];
    const ModelFrame *f = model_frame(&leon, frame);
    const int16_t *rot = model_frame_rot(&leon, frame) + bone * 3;

    glPushMatrix();
    if (b->parent < 0)
        glTranslatef32(f->offset[0], f->offset[1], f->offset[2]);
    else
        glTranslatef32(b->pos[0], b->pos[1], b->pos[2]);
    glRotateXi(rot[0]);
    glRotateYi(rot[1]);
    glRotateZi(rot[2]);

    for (int p = 0; p < leon.header->num_pages; p++) {
        uint32_t off = leon.lists[bone * leon.header->num_pages + p];
        if (off) {
            glBindTexture(0, leon_tex[p]);
            glCallList((const u32 *)((const uint8_t *)leon.data + off));
        }
    }
    for (int c = first_child[bone]; c >= 0; c = next_sibling[c])
        draw_bone(c, frame);
    glPopMatrix(1);
}

static void draw_player(void)
{
    const ModelAnim *a = &leon.anims[player.anim];
    int frame = leon.anim_pool[a->first + player.key];
    glPushMatrix();
    glTranslatef32(player.x, player.y, player.z);
    glRotateYi(player.dir);
    for (int b = 0; b < leon.header->num_bones; b++)
        if (leon.bones[b].parent < 0)
            draw_bone(b, frame);
    glPopMatrix(1);
}

static void draw_masks(void)
{
    if (!have_masks)
        return;
    const RoomCamera *c = &room.cameras[cur_cam];
    glBindTexture(0, mask_tex);
    glPushMatrix();
    // vertices relative to the eye keep them within VTX_16 range
    glTranslatef32(c->from[0], c->from[1], c->from[2]);
    glBegin(GL_QUADS);
    for (int i = 0; i < masks.header->num_quads; i++) {
        const MaskQuad *q = &masks.quads[i];
        int uv[4][2] = { { q->u, q->v }, { q->u + q->w, q->v },
                         { q->u + q->w, q->v + q->h }, { q->u, q->v + q->h } };
        for (int k = 0; k < 4; k++) {
            glTexCoord2t16(inttot16(uv[k][0]), inttot16(uv[k][1]));
            glVertex3v16(q->corner[k][0] - c->from[0], q->corner[k][1] - c->from[1],
                         q->corner[k][2] - c->from[2]);
        }
    }
    glEnd();
    glPopMatrix(1);
}

// ---------------------------------------------------------------- game logic
static int point_in_quad(int32_t x, int32_t z, const RoomSwitch *s)
{
    int sign = 0;
    for (int k = 0; k < 4; k++) {
        int64_t x0 = s->x[k], z0 = s->z[k], x1 = s->x[(k + 1) & 3], z1 = s->z[(k + 1) & 3];
        int64_t c = (x1 - x0) * (z - z0) - (z1 - z0) * (x - x0);
        if (c != 0) {
            if (sign == 0)
                sign = c > 0 ? 1 : -1;
            else if ((c > 0) != (sign > 0))
                return 0;
        }
    }
    return 1;
}

static int blocked(int32_t x, int32_t z)
{
    for (int i = 0; i < room.header->num_boxes; i++) {
        const RoomBox *b = &room.boxes[i];
        if (!(b->floors & 1))
            continue;
        if (x > b->x - PLAYER_RADIUS && x < b->x + b->w + PLAYER_RADIUS &&
            z > b->z - PLAYER_RADIUS && z < b->z + b->d + PLAYER_RADIUS)
            return 1;
    }
    return 0;
}

static void check_camera_switch(void)
{
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if (s->cam0 != cur_cam || (s->role & SWITCH_VIEW_AREA) || s->cam1 == cur_cam)
            continue;
        if (s->floor != 255 && s->floor != 0)
            continue;
        if (point_in_quad(player.x, player.z, s)) {
            set_camera(s->cam1);
            return;
        }
    }
}

static void set_anim(int anim)
{
    if (player.anim != anim) {
        player.anim = anim;
        player.key = 0;
        player.tick = 0;
    }
}

static void move_forward(int32_t dist)
{
    int32_t dx = (dist * cosLerp(player.dir)) >> 12;
    int32_t dz = -((dist * sinLerp(player.dir)) >> 12);
    if (!blocked(player.x + dx, player.z))
        player.x += dx;
    if (!blocked(player.x, player.z + dz))
        player.z += dz;
}

static void update_player(void)
{
    uint32_t held = keysHeld();
    PlayerState st = ST_IDLE;
    if (held & KEY_UP)
        st = (held & KEY_B) ? ST_RUN : ST_WALK;
    else if (held & KEY_DOWN)
        st = ST_BACK;
    player.state = st;

    int turn = st == ST_IDLE ? TURN_IDLE : st == ST_RUN ? TURN_RUN : TURN_WALK;
    if (held & KEY_RIGHT)
        player.dir += turn;
    if (held & KEY_LEFT)
        player.dir -= turn;

    set_anim(st == ST_RUN ? ANIM_RUN : (st == ST_IDLE ? ANIM_IDLE : ANIM_WALK));

    // RE2 animates at 30 Hz: one keyframe every second DS frame.
    if (++player.tick < 2)
        return;
    player.tick = 0;

    const ModelAnim *a = &leon.anims[player.anim];
    int prev = player.key;
    if (st == ST_BACK)
        player.key = (player.key + a->count - 1) % a->count;
    else
        player.key = (player.key + 1) % a->count;

    if (st != ST_IDLE) {
        // root motion: cumulative distance along +X between the two keyframes
        int k0 = st == ST_BACK ? player.key : prev;
        int k1 = st == ST_BACK ? prev : player.key;
        int32_t s0 = k1 == 0 ? 0 : model_frame(&leon, leon.anim_pool[a->first + k0])->speed[0];
        int32_t s1 = model_frame(&leon, leon.anim_pool[a->first + k1])->speed[0];
        int32_t step = s1 - s0;
        if (step < 0)
            step = s1;
        move_forward(st == ST_BACK ? -step : step);
        check_camera_switch();
    }
}

// ---------------------------------------------------------------- main
int main(void)
{
    videoSetMode(MODE_5_3D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    // Default slots only: libnds derives the texture slot from the bank
    // number (B = slot 1, D = slot 3) when it uploads.
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankD(VRAM_D_TEXTURE);
    vramSetBankE(VRAM_E_TEX_PALETTE);
    consoleDemoInit(); // sub screen text, uses VRAM C

    if (!nitroFSInit(NULL))
        fatal("NitroFS init failed");

    bg_id = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    bgSetPriority(bg_id, 1);
    bgSetPriority(0, 0);

    glInit();
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_ANTIALIAS);
    glClearColor(0, 0, 0, 0); // transparent: the BG3 background shows through
    glClearPolyID(63);
    glClearDepth(GL_MAX_DEPTH);
    glViewport(0, 0, 255, 191);

    if (!room_load(&room, "nitro:/room/" RDT_NAME ".bin"))
        fatal("room/" RDT_NAME ".bin missing");
    if (!model_load(&leon, "nitro:/model/leon.bin"))
        fatal("model/leon.bin missing");
    build_hierarchy();
    upload_leon_textures();

    player.x = room.header->spawn[0];
    player.y = room.header->spawn[1];
    player.z = room.header->spawn[2];
    player.dir = room.header->spawn_dir;
    player.anim = -1;
    set_anim(ANIM_IDLE);
    set_camera(room.header->start_camera);

    printf("\x1b[1;1HRE2 DS prototype - room 100\n");
    printf("D-Pad: walk/turn, B: run\n");

    // pmMainLoop() services calico's power management (lid, reset, ARM7 messages);
    // the devkitPro examples call it every frame before reading input.
    while (pmMainLoop()) {
        scanKeys();
        update_player();

        setup_projection(&room.cameras[cur_cam]);
        // Textures are modulated with the vertex colour: keep it white.
        glColor3b(255, 255, 255);
        glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
        draw_player();
        glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(2));
        draw_masks();
        glFlush(0);

        printf("\x1b[4;1Hcam %d  pos %6ld %6ld   \n", cur_cam, (long)player.x, (long)player.z);
        printf("dir %5d  anim %2d key %3d   \n", player.dir & 0x7FFF, player.anim, player.key);
        printf("keys %04lx  raw %03x   \n", (unsigned long)keysHeld(), (~REG_KEYINPUT) & 0x3FF);
        printf("tex %d/%d  masks %d (%d quads, tex %d)   \n", tex_ok, leon.header->num_pages,
               have_masks, have_masks ? masks.header->num_quads : 0, mask_ok);
        swiWaitForVBlank();
    }
    return 0;
}
