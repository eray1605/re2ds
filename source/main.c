// re2ds - Resident Evil 2 for the Nintendo DS (work in progress).
//
// Main screen: pre-rendered background on BG3 (8 bpp bitmap, double-buffered
// in VRAM A) with the 3D layer (BG0) on top: Leon plus the foreground masks
// as depth-tested quads.
// Sub screen: debug text and the room menu (SELECT).
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
#include "script.h"
#include "door.h"
#include "enemy.h"
#include "sound.h"

// Leon's first playable room and the camera the converter picked a spawn for
#define START_STAGE 1
#define START_ROOM 0x00

// Animation numbers in leon.bin: 10 PLD animations, then the PL00W00 set.
enum { ANIM_WALK = 10, ANIM_RUN = 11, ANIM_IDLE = 12 };
// Special moves (player states 8 and 9, pl_move_08 / pl_move_09 at
// 0x8003b6e4 / 0x8003ba78) use the room's own animation set (RBJ, see
// tools/rbj2ds.py): room/<rdt>_rbj.bin holds climb, jump, fall, land.
enum { RBJ_CLIMB = 0, RBJ_JUMP = 1, RBJ_FALL = 2, RBJ_LAND = 3 };
#define JUMP_FALL_KEY 13    // the jump turns into the fall at frame 0x0E

#define LEVEL_HEIGHT 1800   // one floor level (0x708): climb and jump height
#define CLIMB_REACH 700     // probe distance in front of Leon for climb / jump boxes

// Game logic rate. The PS1 game waits VSync(2) per gameplay frame (0x80013c94
// sets the wait to 2), so it runs at 25 Hz on PAL (SLES-00972, our disc) and
// 30 Hz on NTSC. One animation keyframe and one root-motion step per tick.
#define GAME_HZ 25
#define DS_HZ 60

// Turn speeds per game tick in RE2 units (4096 = 360 degrees), Fine state
// (re2ds-notes/player-control.md). Applied per DS frame scaled to DS units.
enum { TURN_IDLE = 80, TURN_WALK = 40, TURN_RUN = 56 };
#define TURN_PER_FRAME(t) ((t) * 8 * GAME_HZ / DS_HZ)

#define PLAYER_RADIUS 400
#define DOOR_REACH 600      // action button probe distance in front of Leon
#define FADE_STEPS 8
#define MASK_SCALE 4        // see draw_masks        // frames per fade (brightness 0 -> -16)

// The background bitmap is 256x256 at 8 bpp = 64 KB; VRAM A (128 KB) holds
// two of them. Map base unit for bitmaps is 16 KB.
#define BG_BASE_FRONT 0
#define BG_BASE_BACK 4

typedef enum { ST_IDLE, ST_WALK, ST_RUN, ST_BACK, ST_CLIMB, ST_JUMP, ST_FALL, ST_LAND } PlayerState;

typedef struct {
    uint8_t stage, room;
} RoomRef;

static Room room;
static int cur_stage, cur_room;
static char bg_prefix[16];    // e.g. "room10a"
static Model leon;
static Model rbj;
static int leon_vertices;           // per frame, for the vertex budget (enemy.c)                   // the room's special-move animations (may be absent)
static Masks masks;
static int have_masks;
static int leon_tex[3];
static int mask_tex = -1;
static int bg_id;
static void *room_bank;             // the room's sound bank (footsteps, room effects)
static int bg_base = BG_BASE_FRONT;
static int cur_cam = -1;
static int8_t first_child[64], next_sibling[64];

static const RoomRef *room_index;
static int room_count;
static int menu_open, menu_sel;
static const Aot *near_aot;         // trigger the action button would activate
static uint32_t pending_keys;       // keys pressed since the last game tick
static int game_tick_acc;           // accumulates GAME_HZ per DS frame
static const char *message_text;    // open message, NULL = none
static uint8_t lock_open[128];      // door locks opened (key id & 0x7F)

static struct {
    int32_t x, y, z;
    int16_t dir;
    PlayerState state;
    int anim, key;
    int slope;                // RoomBox index of the slope/stairs Leon is on, -1 = none
    int32_t fall_speed;       // jump down: rises by 1 per tick, y += speed * 30 (pl_move_09)
    int32_t ground;           // jump down: height to land on
    const Model *set;         // animation set: &leon or &rbj
    int32_t slope_base;       // height of that slope's bottom edge
} player;

static void render_frame(void);

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
        glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_128, TEXTURE_SIZE_256, 0, TEXGEN_TEXCOORD, page + 512);
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
    script_camera_changed(cur_cam, cam);
    cur_cam = cam;

    // Background: unpack into the hidden buffer, then flip buffer and palette
    // together in vblank so the cut is clean.
    snprintf(path, sizeof(path), "nitro:/bg/%s_cam%02d.bin", bg_prefix, cam);
    BgHeader *bg = asset_load(path, NULL);
    if (!bg) {
        // a few rooms (e.g. 30C) repeat one camera and store a single image
        snprintf(path, sizeof(path), "nitro:/bg/%s_cam00.bin", bg_prefix);
        bg = asset_load(path, NULL);
    }
    if (!bg || bg->magic != BG_MAGIC)
        fatal(path);
    int back = bg_base == BG_BASE_FRONT ? BG_BASE_BACK : BG_BASE_FRONT;
    decompress(bg + 1, (u16 *)BG_GFX + 0x2000 * back, LZ77Vram);
    swiWaitForVBlank();
    bgSetMapBase(bg_id, back);
    for (int i = 1; i < 256; i++)
        BG_PALETTE[i] = bg->palette[i];
    bg_base = back;
    asset_free(bg);

    // masks for this camera (optional)
    if (have_masks)
        asset_free(masks.data);
    if (mask_tex >= 0) {
        glDeleteTextures(1, &mask_tex);
        mask_tex = -1;
    }
    snprintf(path, sizeof(path), "nitro:/mask/%s_cam%02d.bin", bg_prefix, cam);
    have_masks = masks_load(&masks, path);
    if (have_masks) {
        DC_FlushRange(masks.data, 512 + sizeof(MaskHeader) + 256 * masks.header->tex_height);
        glGenTextures(1, &mask_tex);
        glBindTexture(0, mask_tex);
        glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_256, tex_size_enum(masks.header->tex_height), 0,
                     TEXGEN_TEXCOORD | GL_TEXTURE_COLOR0_TRANSPARENT, masks.pixels);
        glColorTableEXT(0, 0, 256, 0, 0, masks.palette);
    }
}

// Replaces the current room; returns 0 (and keeps the old one) if the file is
// missing. The caller places the player and calls set_camera().
static int load_room(int stage, int id)
{
    char path[48];
    snprintf(path, sizeof(path), "nitro:/room/room%d%02x0.bin", stage, id);
    Room next;
    if (!room_load(&next, path))
        return 0;
    if (room.data)
        asset_free(room.data);
    room = next;
    cur_stage = stage;
    cur_room = id;
    snprintf(bg_prefix, sizeof(bg_prefix), "room%d%02x", stage, id);
    cur_cam = -1;
    enemy_room_reset(); // the new room's scripts spawn its enemies
    // room sound bank (the previous one may still play a short step: free it
    // only after the new one is loaded)
    void *old_bank = room_bank;
    snprintf(path, sizeof(path), "nitro:/sound/room%d%02x0.bin", stage, id);
    room_bank = sound_bank_load(path);
    if (rbj.data)
        asset_free(rbj.data);
    rbj.data = NULL;
    snprintf(path, sizeof(path), "nitro:/room/room%d%02x0_rbj.bin", stage, id);
    if (!model_load(&rbj, path) && rbj.data) {
        asset_free(rbj.data);
        rbj.data = NULL;
    }
    if (old_bank)
        sound_bank_free(old_bank);
    near_aot = NULL;
    return 1;
}

static void load_room_index(void)
{
    uint8_t *data = asset_load("nitro:/room/index.bin", NULL);
    if (!data)
        fatal("room/index.bin missing");
    room_count = data[0] | data[1] << 8;
    room_index = (const RoomRef *)(data + 2);
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
// Camera like the PS1: lookAt(from, to) with up = -Y, projection onto a
// 320x240 screen at distance `screen`, same vertical field of view on 256x192.
// gluLookAtf32 in float: the fixed-point version breaks down for cameras that
// look almost straight down (room 102, camera 4).
void setup_camera(const int32_t from[3], const int32_t to[3], int screen)
{
    float fovy = 2.0f * atanf(120.0f / (float)screen) * 180.0f / 3.14159265f;
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(fovy, 256.0f / 192.0f, 0.05f, 16.0f);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    float f[3], side[3], up[3], len;
    for (int k = 0; k < 3; k++)
        f[k] = (float)(from[k] - to[k]);
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int k = 0; k < 3; k++)
        f[k] /= len;
    // side = up x forward with up = (0, -1, 0)
    side[0] = -f[2];
    side[1] = 0.0f;
    side[2] = f[0];
    len = sqrtf(side[0] * side[0] + side[2] * side[2]);
    side[0] /= len;
    side[2] /= len;
    up[0] = f[1] * side[2] - f[2] * side[1];
    up[1] = f[2] * side[0] - f[0] * side[2];
    up[2] = f[0] * side[1] - f[1] * side[0];
    float eye[3] = { (float)from[0], (float)from[1], (float)from[2] };
    for (int k = 0; k < 3; k++) {
        MATRIX_MULT4x3 = floattof32(side[k]);
        MATRIX_MULT4x3 = floattof32(up[k]);
        MATRIX_MULT4x3 = floattof32(f[k]);
    }
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * side[0] + eye[1] * side[1] + eye[2] * side[2]);
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * up[0] + eye[1] * up[1] + eye[2] * up[2]);
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2]);
}

// Distance along the view direction if (x, y, z) is within the current
// camera's view (with a margin in world units), else -1. Used to skip
// enemies that are off screen.
int camera_sees(int32_t x, int32_t y, int32_t z, int32_t margin)
{
    const RoomCamera *c = &room.cameras[cur_cam];
    float f[3], r[3], d[3], len, v[3];
    for (int k = 0; k < 3; k++)
        f[k] = (float)(c->to[k] - c->from[k]);
    len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (int k = 0; k < 3; k++)
        f[k] /= len;
    r[0] = f[2];  // right = forward x (0, -1, 0)
    r[1] = 0.0f;
    r[2] = -f[0];
    len = sqrtf(r[0] * r[0] + r[2] * r[2]);
    r[0] /= len;
    r[2] /= len;
    d[0] = f[1] * r[2] - f[2] * r[1];
    d[1] = f[2] * r[0] - f[0] * r[2];
    d[2] = f[0] * r[1] - f[1] * r[0];
    v[0] = (float)(x - c->from[0]);
    v[1] = (float)(y - c->from[1]);
    v[2] = (float)(z - c->from[2]);
    float depth = v[0] * f[0] + v[1] * f[1] + v[2] * f[2];
    if (depth < 100.0f)
        return -1;
    float sx = (v[0] * r[0] + v[1] * r[1] + v[2] * r[2]) * c->screen / depth;
    float sy = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) * c->screen / depth;
    float m = (float)margin * c->screen / depth;
    if (sx < -160 - m || sx > 160 + m || sy < -120 - m || sy > 120 + m)
        return -1;
    return (int32_t)depth;
}

static void setup_projection(const RoomCamera *c)
{
    setup_camera(c->from, c->to, c->screen);
}

static void draw_bone(int bone, int frame)
{
    const ModelBone *b = &leon.bones[bone];
    const ModelFrame *f = model_frame(player.set, frame);
    const int16_t *rot = model_frame_rot(player.set, frame) + bone * 3;

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
    const ModelAnim *a = &player.set->anims[player.anim];
    int frame = ANIM_FRAME(player.set->anim_pool[a->first + player.key]);
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
    // Vertices relative to the eye, divided by MASK_SCALE: VTX_16 holds
    // +-32767 and some masks lie up to ~44000 units from the eye.
    glTranslatef32(c->from[0], c->from[1], c->from[2]);
    glScalef32(inttof32(MASK_SCALE), inttof32(MASK_SCALE), inttof32(MASK_SCALE));
    glBegin(GL_QUADS);
    for (int i = 0; i < masks.header->num_quads; i++) {
        const MaskQuad *q = &masks.quads[i];
        int uv[4][2] = { { q->u, q->v }, { q->u + q->w, q->v },
                         { q->u + q->w, q->v + q->h }, { q->u, q->v + q->h } };
        for (int k = 0; k < 4; k++) {
            glTexCoord2t16(inttot16(uv[k][0]), inttot16(uv[k][1]));
            glVertex3v16((q->corner[k][0] - c->from[0]) / MASK_SCALE,
                         (q->corner[k][1] - c->from[1]) / MASK_SCALE,
                         (q->corner[k][2] - c->from[2]) / MASK_SCALE);
        }
    }
    glEnd();
    glPopMatrix(1);
}

static void render_frame(void)
{
    setup_projection(&room.cameras[cur_cam]);
    // Textures are modulated with the vertex colour: keep it white.
    glColor3b(255, 255, 255);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(1));
    draw_player();
    enemy_draw(leon_vertices + (have_masks ? 4 * masks.header->num_quads : 0));
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(2));
    draw_masks();
    glFlush(0);
}

// Fades the main screen to black (dir = 1) or back (dir = -1), rendering
// every frame so the 3D layer stays valid.
static void fade(int dir)
{
    for (int i = 1; i <= FADE_STEPS; i++) {
        int level = 16 * i / FADE_STEPS;
        setBrightness(1, dir > 0 ? -level : level - 16);
        render_frame();
        swiWaitForVBlank();
    }
}

// ---------------------------------------------------------------- game logic
static int point_in_quad(int32_t x, int32_t z, const int16_t *qx, const int16_t *qz)
{
    int sign = 0;
    for (int k = 0; k < 4; k++) {
        int64_t x0 = qx[k], z0 = qz[k], x1 = qx[(k + 1) & 3], z1 = qz[(k + 1) & 3];
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

static int player_floor(void)
{
    int f = (-player.y - FLOOR_HEIGHT / 2) / -FLOOR_HEIGHT;
    return f < 0 ? 0 : f > 31 ? 31 : f;
}

static int is_slope(const RoomBox *b)
{
    return b->shape == SHAPE_SLOPE || b->shape == SHAPE_STAIRS;
}

// How far a circle of PLAYER_RADIUS at (x, z) reaches into box b (0 = free).
static int32_t penetration(const RoomBox *b, int32_t x, int32_t z)
{
    int32_t in_x = x - (b->x - PLAYER_RADIUS), out_x = b->x + b->w + PLAYER_RADIUS - x;
    int32_t in_z = z - (b->z - PLAYER_RADIUS), out_z = b->z + b->d + PLAYER_RADIUS - z;
    if (in_x <= 0 || out_x <= 0 || in_z <= 0 || out_z <= 0)
        return 0;
    int32_t d = in_x < out_x ? in_x : out_x;
    if (in_z < d)
        d = in_z;
    if (out_z < d)
        d = out_z;
    return d;
}

// A step is blocked only if it pushes Leon deeper into an obstacle, so he
// can always walk out of one he starts in (door exits lie close to walls).
static int blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z)
{
    uint32_t floor_bit = 1u << player_floor();
    for (int i = 0; i < room.header->num_boxes; i++) {
        const RoomBox *b = &room.boxes[i];
        if (!(b->floors & floor_bit) || is_slope(b) || !(b->flag & BOX_PLAYER))
            continue;
        int32_t depth = penetration(b, x, z);
        if (depth > 0 && depth >= penetration(b, from_x, from_z))
            return 1;
    }
    return 0;
}

// Height on slopes and stairs (OpenBiohazard2 game/player.go): the box rises
// by ((type >> 6) & 31) floor heights from its bottom edge, which
// (type >> 4) & 3 selects (0: x min, 1: x max, 2: z min, 3: z max). The bottom
// height is taken when Leon steps onto the box.
static void update_height(void)
{
    for (int i = 0; i < room.header->num_boxes; i++) {
        const RoomBox *b = &room.boxes[i];
        if (!is_slope(b) || b->w == 0 || b->d == 0)
            continue;
        if (player.x < b->x || player.x > b->x + b->w || player.z < b->z || player.z > b->z + b->d)
            continue;
        int32_t frac;   // 0..4096 from the bottom edge
        switch ((b->type >> 4) & 3) {
        case 0: frac = ((player.x - b->x) << 12) / b->w; break;
        case 1: frac = ((b->x + b->w - player.x) << 12) / b->w; break;
        case 2: frac = ((player.z - b->z) << 12) / b->d; break;
        default: frac = ((b->z + b->d - player.z) << 12) / b->d; break;
        }
        int32_t rise = ((b->type >> 6) & 31) * FLOOR_HEIGHT;
        if (player.slope != i) {
            player.slope = i;
            player.slope_base = player.y - ((rise * frac) >> 12);
        }
        player.y = player.slope_base + ((rise * frac) >> 12);
        return;
    }
    player.slope = -1;
}

// Camera switching. Zones are given for the room's normal cameras; scripts
// can show another picture for a camera with cut_replace (room 107 shows 6 and
// 7 instead of 0 and 1 later in the story), so zones match on the replaced
// number. About a third of all cameras have no zones at all: cutscene cameras
// that only scripts switch to (cut_chg) and back.
static int camera_has_zones(int cam)
{
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if (!(s->role & SWITCH_VIEW_AREA) && (s->cam0 == cam || s->cam1 == cam) && s->cam0 != s->cam1)
            return 1;
    }
    return 0;
}

static void check_camera_switch(void)
{
    if (!cut_auto_enabled())
        return;
    int floor = player_floor();
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if ((s->role & SWITCH_VIEW_AREA) || cut_replace(s->cam0) != cur_cam)
            continue;
        if (s->floor != 255 && s->floor != floor)
            continue;
        if (point_in_quad(player.x, player.z, s->x, s->z)) {
            int cam = cut_replace(s->cam1);
            if (cam != cur_cam && cam < room.header->num_cameras)
                set_camera(cam);
            return;
        }
    }

    // Leon outside the view area of the shown camera (a cutscene camera the
    // unfinished cutscene never switched back from): take a normal camera
    // whose view area he stands in.
    int in_view = 0, other = -1;
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if (!(s->role & SWITCH_VIEW_AREA) || (s->floor != 255 && s->floor != floor))
            continue;
        if (!point_in_quad(player.x, player.z, s->x, s->z))
            continue;
        if (cut_replace(s->cam0) == cur_cam)
            in_view = 1;
        else if (other < 0 && camera_has_zones(s->cam0))
            other = cut_replace(s->cam0);
    }
    if (!in_view && other >= 0 && other < room.header->num_cameras)
        set_camera(other);
}

static void set_anim(int anim)
{
    if (player.anim != anim || player.set != &leon) {
        player.set = &leon;
        player.anim = anim;
        player.key = 0;
    }
}

static void set_special_anim(int anim)
{
    player.set = &rbj;
    player.anim = anim;
    player.key = 0;
}

static void place_player(int32_t x, int32_t y, int32_t z, int16_t dir, int cam)
{
    player.x = x;
    player.y = y;
    player.z = z;
    player.dir = dir;
    player.slope = -1;
    player.anim = -1;
    set_anim(ANIM_IDLE);
    cam = cut_replace(cam);
    set_camera(cam < room.header->num_cameras ? cam : 0);
}

// ---------------------------------------------------------------- script hooks
void game_show_message(const char *text)
{
    message_text = text;
    consoleClear();
    printf("\x1b[2;0H");
    // word wrap at 32 columns
    int col = 0;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            printf("\n");
            col = 0;
            continue;
        }
        if (*p == ' ' && col > 0) {
            int len = 0;
            while (p[1 + len] && p[1 + len] != ' ' && p[1 + len] != '\n')
                len++;
            if (col + 1 + len > 31) {
                printf("\n");
                col = 0;
                continue;
            }
        }
        printf("%c", *p);
        col++;
    }
    printf("\n\n\n                       [A]\n");
}

int game_message_open(void) { return message_text != NULL; }

void game_cut_change(int cam)
{
    if (cam >= 0 && cam < room.header->num_cameras && cam != cur_cam)
        set_camera(cam);
}

void game_set_player_pos(int32_t x, int32_t y, int32_t z)
{
    player.x = x;
    player.y = y;
    player.z = z;
    player.slope = -1;
}

void game_set_player_dir(int16_t dir_re2) { player.dir = dir_re2 * 8; }

void game_spawn_enemy(int slot, int type, int variant, int x, int y, int z, int dir_re2)
{
    enemy_spawn(slot, type, variant, x, y, z, dir_re2);
}

// room collision for enemies (same rule as the player, on the floor of y)
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y)
{
    int32_t saved = player.y;
    player.y = y; // player_floor() reads it
    int b = blocked(from_x, from_z, x, z);
    player.y = saved;
    return b;
}

void game_play_sound(int bank, int sound)
{
    if (bank == 2)
        sound_play(room_bank, sound, 110, 64);
}

void game_set_box_flag(int box, int flag)
{
    if (box >= 0 && box < room.header->num_boxes)
        ((RoomBox *)room.boxes)[box].flag = flag; // room data lives in RAM
}

// The init script may test the arrival camera (VAR_CUT), e.g. room 107.
static void enter_room(int stage, int id, int arrival_cam)
{
    load_room(stage, id);
    script_camera_changed(-1, arrival_cam);
    script_room_enter(&room);
}

static void warp_to(int index)
{
    const RoomRef *r = &room_index[index];
    fade(1);
    enter_room(r->stage, r->room, -1);
    const RoomHeader *h = room.header;
    place_player(h->spawn[0], h->spawn[1], h->spawn[2], h->spawn_dir, h->start_camera);
    fade(-1);
}

// ---------------------------------------------------------------- triggers
static void use_door(const Aot *a)
{
    const uint8_t *d = a->data;
    int16_t nx = d[0] | d[1] << 8, ny = d[2] | d[3] << 8, nz = d[4] | d[5] << 8;
    int16_t ndir = d[6] | d[7] << 8;
    // copy everything now: enter_room() runs the new room's scripts, which
    // overwrite this AOT slot (a->data) with their own AOTs
    int stage = d[8] + 1, id = d[9], cam = d[10], key_id = d[15], key_type = d[16];
    int door_type = d[12], door_variant = d[13];

    // key id = 0x80 | lock number (shared by both sides of the door), key type
    // = item that opens it. 0xFE / 0xFF mark the two sides of a door that is
    // opened from one side only (which side is which is still a guess).
    if (key_id && !lock_open[key_id & 0x7F]) {
        if (keysHeld() & KEY_R) {
            lock_open[key_id & 0x7F] = 1; // debug: R + A opens every lock
        } else if (key_type == 0xFE) {
            lock_open[key_id & 0x7F] = 1;
            game_show_message("You unlocked the door.");
            return;
        } else if (key_type == 0xFF) {
            game_show_message("It's locked from the other side.");
            return;
        } else if (inventory_has(key_type)) {
            lock_open[key_id & 0x7F] = 1;
            game_show_message("You used the key.");
            return;
        } else {
            game_show_message("It's locked.");
            return;
        }
    }

    // Door sequence (door.c): the PS1 loads the next room while the door
    // program of the door file plays; here the room
    // is loaded behind a black screen first, then the program runs.
    fade(1);
    enter_room(stage, id, cam);
    place_player(nx, ny, nz, ndir * 8, cam);
    // byte 12 = door type; type t loads disc file 0xD7 + t (table 0x80092948),
    // whose sector (table 0x8009146c) is that of DOORtt.DO2 (checked: file
    // 0xD7 starts at LBA 0xBA41 = DOOR00)
    if (door_begin(door_type, door_variant)) {
        bgHide(bg_id);
        while (door_frame())
            swiWaitForVBlank();
        door_end();
        setBrightness(1, -16);
        bgShow(bg_id);
    }
    fade(-1);
}

static void trigger(Aot *a)
{
    static char text[64];
    switch (a->type) {
    case SCE_DOOR:
        use_door(a);
        break;
    case SCE_ITEM: {
        int item = a->data[0] | a->data[1] << 8, amount = a->data[2] | a->data[3] << 8;
        int picked = a->data[4] | a->data[5] << 8;
        inventory_add(item, amount);
        flag_set(FLAGS_ITEM_PICKED, picked, 1);
        a->active = 0;
        snprintf(text, sizeof(text), "Item %d picked up (x%d).", item, amount);
        game_show_message(text);
        break;
    }
    case SCE_MESSAGE: {
        const char *m = room_message(&room, a->data[0]);
        if (m && *m)
            game_show_message(m);
        break;
    }
    case SCE_EVENT:
        if (a->data[2] == 0x18) // gosub
            script_start_event(a->data[3]);
        break;
    default:
        break;
    }
}

// Triggers without AOT_SAT_MANUAL fire when Leon walks into them; manual ones
// need A while Leon stands in or faces the area.
static void check_aots(uint32_t pressed)
{
    int floor = player_floor();
    int32_t px = player.x + ((DOOR_REACH * cosLerp(player.dir)) >> 12);
    int32_t pz = player.z - ((DOOR_REACH * sinLerp(player.dir)) >> 12);
    near_aot = NULL;
    for (int i = 0; i < NUM_AOTS; i++) {
        Aot *a = &aots[i];
        if (!a->active || !(a->sat & AOT_SAT_PLAYER) || (a->floor != 255 && a->floor != floor))
            continue;
        int inside = point_in_quad(player.x, player.z, a->x, a->z);
        if (!(a->sat & AOT_SAT_MANUAL)) {
            int entered = inside && !a->inside;
            a->inside = inside;
            if (entered) {
                trigger(a);
                return;
            }
            continue;
        }
        if (inside || point_in_quad(px, pz, a->x, a->z)) {
            near_aot = a;
            if (pressed & KEY_A) {
                trigger(a);
                return;
            }
        }
    }
}

// ---------------------------------------------------------------- player
static void move_forward(int32_t dist)
{
    int32_t dx = (dist * cosLerp(player.dir)) >> 12;
    int32_t dz = -((dist * sinLerp(player.dir)) >> 12);
    if (!blocked(player.x, player.z, player.x + dx, player.z))
        player.x += dx;
    if (!blocked(player.x, player.z, player.x, player.z + dz))
        player.z += dz;
    update_height();
}

// every DS frame: state, animation choice and turning
static void update_player_frame(uint32_t held)
{
    if (player.state >= ST_CLIMB)
        return; // special move in progress: no control
    PlayerState st = ST_IDLE;
    if (held & KEY_UP)
        st = (held & KEY_B) ? ST_RUN : ST_WALK;
    else if (held & KEY_DOWN)
        st = ST_BACK;
    player.state = st;

    int turn = TURN_PER_FRAME(st == ST_IDLE ? TURN_IDLE : st == ST_RUN ? TURN_RUN : TURN_WALK);
    if (held & KEY_RIGHT)
        player.dir += turn;
    if (held & KEY_LEFT)
        player.dir -= turn;

    set_anim(st == ST_RUN ? ANIM_RUN : (st == ST_IDLE ? ANIM_IDLE : ANIM_WALK));
}

// every game tick: next keyframe and its root motion
// Floor sound under Leon (FUN_8004f078): last matching FLR area of his floor.
static int floor_sound(void)
{
    int floor = player_floor();
    for (int i = room.header->num_flr - 1; i >= 0; i--) {
        const RoomFloorSound *f = &room.floor_sounds[i];
        if ((f->floor & 0x80) || (f->floor & 0x7F) != floor)
            continue;
        if (player.x >= f->x && player.x <= f->x + f->w && player.z >= f->z && player.z <= f->z + f->d)
            return f->sound;
    }
    return FLOOR_SOUND_DEFAULT;
}

static void step_forward(int32_t dist)
{
    player.x += (dist * cosLerp(player.dir)) >> 12;
    player.z -= (dist * sinLerp(player.dir)) >> 12;
}

// Root motion between two keyframes of the current animation: forward along
// Leon's facing (speed x) and, if asked, up/down (speed y, -Y is up).
static void special_root_motion(int k0, int k1, int with_y)
{
    const Model *m = player.set;
    const ModelAnim *a = &m->anims[player.anim];
    const ModelFrame *f0 = model_frame(m, ANIM_FRAME(m->anim_pool[a->first + k0]));
    const ModelFrame *f1 = model_frame(m, ANIM_FRAME(m->anim_pool[a->first + k1]));
    step_forward(f1->speed[0] - f0->speed[0]);
    if (with_y)
        player.y += f1->speed[1] - f0->speed[1];
}

// Special moves, one keyframe per game tick like the PS1.
// Climbing (state 8): RBJ climb animation; at frame 29 its root motion moves
// Leon 1000 forward and one level (1800) up onto the box.
// Jumping down (state 9): RBJ jump until frame 14 (1000 forward), then the
// fall pose while y += speed * 30 with speed starting at 5 and rising by one
// per tick, until the floor one level lower; then the landing animation.
static void special_move_tick(void)
{
    const ModelAnim *a = &player.set->anims[player.anim];
    int prev = player.key;
    int last = player.key + 1 >= a->count;
    switch (player.state) {
    case ST_CLIMB:
        if (last) {
            player.state = ST_IDLE;
            set_anim(ANIM_IDLE);
            check_camera_switch();
            break;
        }
        player.key++;
        special_root_motion(prev, player.key, 1);
        break;
    case ST_JUMP:
        if (last || player.key >= JUMP_FALL_KEY) {
            player.state = ST_FALL;
            player.fall_speed = 5;
            set_special_anim(RBJ_FALL);
            break;
        }
        player.key++;
        special_root_motion(prev, player.key, 0);
        break;
    case ST_FALL:
        player.y += player.fall_speed * 30;
        player.fall_speed++;
        if (player.y >= player.ground) {
            player.y = player.ground;
            player.state = ST_LAND;
            set_special_anim(RBJ_LAND);
            sound_play(room_bank, floor_sound() + 1, 100, 64); // landing step
        }
        check_camera_switch();
        break;
    case ST_LAND:
        if (last) {
            player.state = ST_IDLE;
            set_anim(ANIM_IDLE);
        } else {
            player.key++;
        }
        break;
    default:
        break;
    }
}

// Box of the given shape (9 = climb up, 10 = jump down) right in front of
// Leon on his floor, or NULL.
static const RoomBox *box_in_front(int shape)
{
    uint32_t floor_bit = 1u << player_floor();
    int32_t px = player.x + ((CLIMB_REACH * cosLerp(player.dir)) >> 12);
    int32_t pz = player.z - ((CLIMB_REACH * sinLerp(player.dir)) >> 12);
    for (int i = 0; i < room.header->num_boxes; i++) {
        const RoomBox *b = &room.boxes[i];
        if (b->shape != shape || !(b->floors & floor_bit))
            continue;
        if (px >= b->x && px <= b->x + b->w && pz >= b->z && pz <= b->z + b->d)
            return b;
    }
    return NULL;
}

// A pressed with no trigger in reach: climb up or jump down if possible.
static void try_special_move(uint32_t pressed)
{
    if (!(pressed & KEY_A) || player.state >= ST_CLIMB)
        return;
    if (!rbj.data)
        return; // this room has no climb animations
    const RoomBox *b = box_in_front(SHAPE_CLIMB_UP);
    if (b) {
        // face the box square on (the PS1 turns Leon first, pl_move_08 case 1)
        if (player.x < b->x)
            player.dir = 0;           // +X
        else if (player.x > b->x + b->w)
            player.dir = 16384;       // -X
        else if (player.z < b->z)
            player.dir = 24576;       // +Z
        else
            player.dir = 8192;        // -Z
        player.state = ST_CLIMB;
        player.slope = -1;
        set_special_anim(RBJ_CLIMB);
    } else if (box_in_front(SHAPE_JUMP_DOWN)) {
        player.state = ST_JUMP;
        player.slope = -1;
        player.ground = player.y + LEVEL_HEIGHT;
        set_special_anim(RBJ_JUMP);
    }
}

static void update_player_tick(void)
{
    PlayerState st = player.state;
    if (st >= ST_CLIMB) {
        special_move_tick();
        return;
    }
    const ModelAnim *a = &leon.anims[player.anim];
    int prev = player.key;
    if (st == ST_BACK)
        player.key = (player.key + a->count - 1) % a->count;
    else
        player.key = (player.key + 1) % a->count;

    // footsteps: keyframes flagged 0x4000 (0x2000 = other foot, only picks the
    // PS1 voice); the sound depends on the floor (FUN_8005797c)
    uint16_t entry = leon.anim_pool[a->first + player.key];
    if (st != ST_IDLE && (entry & ANIM_FLAG_STEP))
        sound_play(room_bank, floor_sound() + (st == ST_RUN ? 1 : 0), 100, 64);

    if (st != ST_IDLE) {
        // root motion: cumulative distance along +X between the two keyframes
        int k0 = st == ST_BACK ? player.key : prev;
        int k1 = st == ST_BACK ? prev : player.key;
        int32_t s0 = k1 == 0 ? 0 : model_frame(&leon, ANIM_FRAME(leon.anim_pool[a->first + k0]))->speed[0];
        int32_t s1 = model_frame(&leon, ANIM_FRAME(leon.anim_pool[a->first + k1]))->speed[0];
        int32_t step = s1 - s0;
        if (step < 0)
            step = s1;
        move_forward(st == ST_BACK ? -step : step);
        check_camera_switch();
    }
}

// ---------------------------------------------------------------- debug screen
static const char *aot_name(int type)
{
    switch (type) {
    case SCE_DOOR: return "Tuer";
    case SCE_ITEM: return "Item";
    case SCE_MESSAGE: return "Text";
    case SCE_EVENT: return "Event";
    default: return "?";
    }
}

static void print_status(void)
{
    printf("\x1b[0;0HRE2 DS - room %X%02X  cam %d   \n", cur_stage, cur_room, cur_cam);
    printf("pos %6ld %6ld %6ld  \n", (long)player.x, (long)player.y, (long)player.z);
    printf("dir %5d floor %d slope %3d  \n", player.dir & 0x7FFF, player_floor(), player.slope);
    printf("skript: %d threads, %d unbek.  \n", script_threads_running, script_unknown_ops);
    printf("gegner: %d  aufrufe %d  fehler %d  \n", enemy_count(), enemy_spawn_calls, enemy_last_error);
    if (near_aot && near_aot->type == SCE_DOOR)
        printf("A: Tuer -> %X%02X            \n", near_aot->data[8] + 1, near_aot->data[9]);
    else if (near_aot)
        printf("A: %s                 \n", aot_name(near_aot->type));
    else if (player.state < ST_CLIMB && box_in_front(SHAPE_CLIMB_UP))
        printf("A: hochklettern            \n");
    else if (player.state < ST_CLIMB && box_in_front(SHAPE_JUMP_DOWN))
        printf("A: runterspringen          \n");
    else
        printf("                          \n");
    printf("\nD-Pad gehen/drehen, B rennen\nA benutzen, R+A Schloss oeffnen\nSELECT Raum-Menue\n");
}

static void update_menu(uint32_t down)
{
    if (down & KEY_SELECT) {
        menu_open = !menu_open;
        consoleClear();
        if (menu_open) {
            for (int i = 0; i < room_count; i++)
                if (room_index[i].stage == cur_stage && room_index[i].room == cur_room)
                    menu_sel = i;
        }
    }
    if (!menu_open)
        return;
    if (down & KEY_UP)
        menu_sel--;
    if (down & KEY_DOWN)
        menu_sel++;
    if (down & (KEY_LEFT | KEY_L))
        menu_sel -= 10;
    if (down & (KEY_RIGHT | KEY_R))
        menu_sel += 10;
    menu_sel = (menu_sel % room_count + room_count) % room_count;
    if (down & (KEY_A | KEY_B)) {
        menu_open = 0;
        consoleClear();
        if (down & KEY_A)
            warp_to(menu_sel);
        return;
    }

    printf("\x1b[0;0HRaum waehlen (%d Raeume)\n", room_count);
    printf("hoch/runter, L/R +-10\nA springen, B zurueck\n\n");
    int first = menu_sel - 7;
    if (first < 0)
        first = 0;
    if (first > room_count - 16)
        first = room_count - 16 < 0 ? 0 : room_count - 16;
    for (int i = first; i < first + 16 && i < room_count; i++)
        printf("%c Raum %X%02X   \n", i == menu_sel ? '>' : ' ', room_index[i].stage, room_index[i].room);
}

// ---------------------------------------------------------------- main
int main(void)
{
    videoSetMode(MODE_5_3D);
    vramSetBankA(VRAM_A_MAIN_BG_0x06000000);
    // Default slots only: libnds derives the texture slot from the bank
    // number (B = slot 1, D = slot 3) when it uploads.
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankC(VRAM_C_TEXTURE);  // enemy textures
    vramSetBankD(VRAM_D_TEXTURE);
    vramSetBankE(VRAM_E_TEX_PALETTE);
    // sub screen text on VRAM H (32 KB): font tiles at 0, map at 8 KB
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 4, 0, false, true);

    if (!nitroFSInit(NULL))
        fatal("NitroFS init failed");
    sound_init();

    bg_id = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, BG_BASE_FRONT, 0);
    bgSetPriority(bg_id, 1);
    bgSetPriority(0, 0);
    BG_PALETTE[0] = 0;

    glInit();
    glEnable(GL_TEXTURE_2D);
    glEnable(GL_ANTIALIAS);
    glClearColor(0, 0, 0, 0); // transparent: the BG3 background shows through
    glClearPolyID(63);
    glClearDepth(GL_MAX_DEPTH);
    glViewport(0, 0, 255, 191);

    if (!model_load(&leon, "nitro:/model/leon.bin"))
        fatal("model/leon.bin missing");
    build_hierarchy();
    upload_leon_textures();
    leon_vertices = model_vertex_count(&leon);
    load_room_index();

    setBrightness(1, -16);
    enter_room(START_STAGE, START_ROOM, -1);
    if (!room.data)
        fatal("room/room1000.bin missing");
    const RoomHeader *h = room.header;
    place_player(h->spawn[0], h->spawn[1], h->spawn[2], h->spawn_dir, h->start_camera);
    fade(-1);

    // pmMainLoop() services calico's power management (lid, reset, ARM7 messages);
    // the devkitPro examples call it every frame before reading input.
    while (pmMainLoop()) {
        scanKeys();
        uint32_t down = keysDown();
        if (message_text) {
            // play is paused while a message is shown; the closing press is
            // not passed on (it would trigger the same AOT again)
            if (down & (KEY_A | KEY_B)) {
                message_text = NULL;
                consoleClear();
            }
            down = 0;
        } else {
            update_menu(down);
        }
        if (!menu_open && !message_text) {
            pending_keys |= down;
            update_player_frame(keysHeld());
            for (game_tick_acc += GAME_HZ; game_tick_acc >= DS_HZ && !message_text; game_tick_acc -= DS_HZ) {
                update_player_tick();
                enemy_tick(player.x, player.z);
                check_aots(pending_keys);
                if (!near_aot && !message_text)
                    try_special_move(pending_keys);
                pending_keys = 0;
                script_tick();
            }
            if (!message_text)
                print_status();
        }
        render_frame();
        swiWaitForVBlank();
    }
    return 0;
}
