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
#include "sound.h"

// Leon's first playable room and the camera the converter picked a spawn for
#define START_STAGE 1
#define START_ROOM 0x00

// Animation numbers in leon.bin: 10 PLD animations, then the PL00W00 set.
enum { ANIM_WALK = 10, ANIM_RUN = 11, ANIM_IDLE = 12 };

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

typedef enum { ST_IDLE, ST_WALK, ST_RUN, ST_BACK } PlayerState;

typedef struct {
    uint8_t stage, room;
} RoomRef;

static Room room;
static int cur_stage, cur_room;
static char bg_prefix[16];    // e.g. "room10a"
static Model leon;
static Masks masks;
static int have_masks;
static int leon_tex[3];
static int mask_tex = -1;
static int bg_id;
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
    // gluLookAtf32 with up = -Y, but in float: the fixed-point version breaks
    // down for cameras that look almost straight down (room 102, camera 4).
    float f[3], side[3], up[3], len;
    for (int k = 0; k < 3; k++)
        f[k] = (float)(c->from[k] - c->to[k]);
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
    float eye[3] = { (float)c->from[0], (float)c->from[1], (float)c->from[2] };
    for (int k = 0; k < 3; k++) {
        MATRIX_MULT4x3 = floattof32(side[k]);
        MATRIX_MULT4x3 = floattof32(up[k]);
        MATRIX_MULT4x3 = floattof32(f[k]);
    }
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * side[0] + eye[1] * side[1] + eye[2] * side[2]);
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * up[0] + eye[1] * up[1] + eye[2] * up[2]);
    MATRIX_MULT4x3 = (int32_t)-(eye[0] * f[0] + eye[1] * f[1] + eye[2] * f[2]);
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
    if (player.anim != anim) {
        player.anim = anim;
        player.key = 0;
    }
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
    int stage = d[8] + 1, id = d[9], cam = d[10], key_id = d[15], key_type = d[16];

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

    // Door sequence. The PS1 plays the door program of DOORnn.DO2 (byte 12 of
    // the door data, 0x800147e8) while it loads the room: model, camera and
    // sounds. The animation is not ported yet; the sounds are: note 60 = the
    // door opens, 61 = it closes. The bank stays loaded until the next door
    // because the ARM7 may still be playing from it.
    static void *door_bank;
    char path[32];
    if (door_bank)
        sound_bank_free(door_bank);
    snprintf(path, sizeof(path), "nitro:/sound/door%02d.bin", d[12]);
    door_bank = sound_bank_load(path);
    sound_play(door_bank, 60, 127, 64);
    int open_frames = sound_frames(door_bank, 60);

    fade(1);
    enter_room(stage, id, cam);
    place_player(nx, ny, nz, ndir * 8, cam);
    for (int f = FADE_STEPS; f < open_frames; f++)
        swiWaitForVBlank();
    sound_play(door_bank, 61, 127, 64);
    for (int f = 0; f < sound_frames(door_bank, 61) / 2; f++)
        swiWaitForVBlank();
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
static void update_player_tick(void)
{
    PlayerState st = player.state;
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
    if (near_aot && near_aot->type == SCE_DOOR)
        printf("A: Tuer -> %X%02X            \n", near_aot->data[8] + 1, near_aot->data[9]);
    else if (near_aot)
        printf("A: %s                 \n", aot_name(near_aot->type));
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
    vramSetBankD(VRAM_D_TEXTURE);
    vramSetBankE(VRAM_E_TEX_PALETTE);
    consoleDemoInit(); // sub screen text, uses VRAM C

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
                check_aots(pending_keys);
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
