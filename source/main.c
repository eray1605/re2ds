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
#include "movie.h"
#include "roomfx.h"
#include "save.h"
#include "plc.h"
#include "effect.h"
#include "music.h"
#include "menu.h"
#include "sound.h"

// Leon's first playable room and the camera the converter picked a spawn for
#define START_STAGE 1
#define START_ROOM 0x00
// New game: Leon at the burning wreck at the east end of the street (camera
// 0), looking west, where the PS1 starts after the intro. Position chosen by
// eye; the original start data is not located yet. Zombie 0 lies close by
// and gets up two seconds in (room script exec function 2).
#define START_X 19800
#define START_Z 300
#define START_DIR 16384  // DS units: -X
#define START_CAM 0

// Animation numbers in leon.bin: 10 PLD animations, then the PL00W00 set.
// Leon's moves come from the weapon set (PLW, animations from 10 on), three
// per health level Fine / Caution / Danger (tables at 0x80010bc0 ff.): walk
// 0 / 3 / 6, run 1 / 4 / 7, stand 2 / 5 / 8. Hurt, he limps.
#define PLW_FIRST 10
static const uint8_t anim_walk_tbl[3] = { 0, 3, 6 }, anim_run_tbl[3] = { 1, 4, 7 },
                     anim_idle_tbl[3] = { 2, 5, 8 };
// turning per game frame (4096 = 360 degrees): walking, running, backing
// up, on the spot (0x80010bc8, 0x80010bd4, 0x80010bdc, 0x80010be4)
static const uint8_t turn_walk[3] = { 40, 32, 16 }, turn_run[3] = { 56, 48, 24 },
                     turn_back[3] = { 40, 16, 16 }, turn_spot[3] = { 80, 48, 32 };
static int health_level(void);
#define ANIM_WALK (PLW_FIRST + anim_walk_tbl[health_level()])
#define ANIM_RUN (PLW_FIRST + anim_run_tbl[health_level()])
#define ANIM_IDLE (PLW_FIRST + anim_idle_tbl[health_level()])
// Special moves (player states 8 and 9, pl_move_08 / pl_move_09 at
// 0x8003b6e4 / 0x8003ba78) use the room's own animation set (RBJ, see
// tools/rbj2ds.py): in room/<rdt>_rbj.bin (the room's player set) they are
// animations 6..9 when the set has them (rbj_moves).
enum { RBJ_CLIMB = 6, RBJ_JUMP = 7, RBJ_FALL = 8, RBJ_LAND = 9 };
static const uint8_t rbj_move_keys[4] = { 50, 15, 20, 20 };
#define JUMP_FALL_KEY 13    // the jump turns into the fall at frame 0x0E

#define LEVEL_HEIGHT 1800   // one floor level (0x708): climb and jump height
#define CLIMB_REACH 700     // probe distance in front of Leon for climb / jump boxes

// Game logic rate. The PS1 game waits VSync(2) per gameplay frame (0x80013c94
// sets the wait to 2), so it runs at 25 Hz on PAL (SLES-00972, our disc) and
// 30 Hz on NTSC. One animation keyframe and one root-motion step per tick.
#define GAME_HZ 25
#define DS_HZ 60

// Turn speeds per game tick in RE2 units (4096 = 360 degrees), Fine state
// (docs/player-control.md). Applied per DS frame scaled to DS units.
#define TURN_PER_FRAME(t) ((t) * 8 * GAME_HZ / DS_HZ)

#define PLAYER_RADIUS 400
#define DOOR_REACH 600      // action button probe distance in front of Leon
#define FADE_STEPS 8
#define MASK_SCALE 4        // see draw_masks        // frames per fade (brightness 0 -> -16)

// The background bitmap is 256x256 at 8 bpp = 64 KB, exactly VRAM E. A new
// camera's picture is unpacked into RAM and copied over in the vblank that
// shows the first 3D frame of it, so all four big banks hold textures.
#define BG_BYTES (256 * 256)
static uint8_t bg_ram[BG_BYTES] __attribute__((aligned(32)));

typedef enum { ST_IDLE, ST_WALK, ST_RUN, ST_BACK, ST_TURN, ST_AIM, ST_FIRE, ST_RELOAD, ST_CLIMB, ST_JUMP, ST_FALL, ST_LAND,
               ST_SCRIPT, ST_GRABBED, ST_DEAD } PlayerState; // script: plc.c; held / killed: enemy.c

typedef struct {
    uint8_t stage, room;
} RoomRef;

static Room room;
static int cur_stage, cur_room;
static char bg_prefix[16];    // e.g. "room10a"
static Model leon;
static void *room_zones;             // the room's path zones (may be absent)
static Model rbj;                   // the room's animations for Leon (special moves, cutscenes)
static int rbj_moves;                // rbj holds climb / jump / fall / land
// The room's object models (items lying around, ...: tools/obj2ds.py), placed
// by obj_model_set; index = the model's number in the room
#define MAX_OBJECTS 32
#define OBJ_MAGIC 0x31424F52  // "ROB1"
// Their textures (up to 190 KB in a room) are uploaded only for placed models,
// after the room's first enemies have theirs, items first, within a budget:
// an enemy without texture memory is invisible.
#define OBJ_TEX_BUDGET (96 * 1024)
static uint8_t *obj_data;             // room<id>_obj.bin
static int obj_count, obj_tex_count;
static int obj_tex[MAX_OBJECTS];
static uint8_t obj_tex_state[MAX_OBJECTS]; // 0 not uploaded, 1 uploaded, 2 no room for it
static int obj_tex_used;              // bytes uploaded in this room
static int objects_ready;             // the room's init scripts have run
static struct {
    uint8_t placed, hidden;
    uint8_t type;      // obj_model_set ins[2]
    uint16_t flags;    // ins[10] | 1 (0x10: drawn in every camera)
    int16_t pos[3], dir[3];
} objects[MAX_OBJECTS];
static Model rbj_enemy[MAX_ENEMIES];  // the room's animations per enemy slot (Kendo in 101)
static Masks masks;
static int have_masks;
static int leon_tex[3];
static int mask_tex = -1;
static int bg_id;
static void *room_bank;             // the room's sound bank (footsteps, room effects)
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
static SaveSlot saves[SAVE_SLOTS];  // re2ds.sav as last read
static InvSlot item_box[BOX_SLOTS]; // the item box (the same in every save room)
#define FIRST_FILE 0x68              // the documents: items 0x68-0x80
#define NUM_FILES 25
static uint32_t files_found;         // bit n: document FIRST_FILE + n
static uint16_t save_count;         // saves of this game so far
static int last_slot;               // the slot saved to or loaded from

static struct {
    int32_t x, y, z;
    int16_t dir;
    PlayerState state;
    int anim, key;
    int slope;                // RoomBox index of the slope/stairs Leon is on, -1 = none
    int32_t fall_speed;       // jump down: rises by 1 per tick, y += speed * 30 (pl_move_09)
    int32_t ground;           // jump down: height to land on
    const Model *set;         // animation set: &leon, &rbj or an enemy's (held)
    int32_t slope_base;       // height of that slope's bottom edge
    int16_t hp;               // 200 at the start (PS1 obj+0x156)
    uint8_t aim_raised;       // weapon up (phase 1 of pl_move_05_aim)
    uint8_t aim_posture;      // 0 straight, 1 up, 2 down
    int8_t aim_target;        // enemy the auto-aim turns to, -1 none
    int32_t aim_x, aim_z;
    uint8_t shot;             // fired this tick (zombies hear it)
    uint8_t saved_once;       // survived a bite once with 0 HP (PS1 0x800c7948 & 0x1000)
    Plc plc;                  // the room script moves him (ST_SCRIPT)
} player;
#define PLAYER_HP 200

// Fine / Caution / Danger as the PS1 works it out (hp < 101, hp < 21)
static int health_level(void) { return player.hp < 21 ? 2 : player.hp < 101 ? 1 : 0; }

static void *voice_bank;            // Leon's voice (CORE00, PS1 bank 4)
static void *enemy_bank;            // the room's enemy sounds (ENEMSE set, bank 3)
static uint8_t *enemy_pairs;        // voice kind pairs per set (enemse2ds.py)
// Leon's death (COMMON/BIN/DIEDEMO.BIN, loaded at 0x80190000; its states are
// in table 0x80193560 and run once per game tick):
//   wait     a delay by killer (table 0x8019338c: zombies 95 ticks); the
//            common voice clip 0 starts (xa_on 1, 0: the main loop sets
//            0x800c77b4 bit 4 when Leon's HP drop below 0)
//   flash    the screen fades to white, faster and faster, and stays white
//            while that clip still plays (0x801902fc)
//   orbit    black background; a free camera 3000 away and 1000 above Leon's
//            hip circles him and closes in (0x80191200, parameters per
//            killer in 0x801933ac, the zombie row used here)
//   picture  DIED00.DIE behind him, his light flashes red (0x80190e88)
//   fade     the camera rises and spins, the light dies, the screen fades
//            out (0x80191080); the PS1 then goes to the title, here a new
//            game starts
// The PS1 draws the picture as a 16 x 16 grid that ripples; here it fades in.
enum { DIE_OFF, DIE_WAIT, DIE_FLASH, DIE_ORBIT, DIE_PICTURE, DIE_FADE };
#define DIE_DELAY 95
#define DIE_ORBIT_TICKS 91          // row field 20
#define DIE_SCREEN 290              // projection distance of the free camera (not traced)
static struct {
    int step, t;
    int32_t flash, flash_v, flash_a; // 0x80193706 / 04 / 00
    int32_t radius, shrink;         // 0x80193728 / 30
    int angle, spin;                // 0x8019372c / 2e, 4096 = 360 degrees
    int32_t rise, rise_acc;         // 0x80193700 / 36: eye height speed (-Y is up)
    int32_t eye[3], at[3];          // 0x8019370c, 0x80193718
    int ambient, light[3];          // the room light the PS1 changes (obj + 4, + 10)
} die;
static RoomCamera die_cam;          // the free camera while die.step >= DIE_ORBIT
static uint16_t die_palette[256];

static void render_frame(void);
static int floor_sound(void);
static void check_camera_switch(void);
static void update_height(void);
static void update_occlusion(void);

// ---------------------------------------------------------------- loading
// Writes into texture VRAM (banks A-D), which the CPU can only reach while
// they are mapped as LCDC, 16 bits at a time. dst and bytes even.
static void texture_write(void *dst, const uint8_t *src, int bytes)
{
    if (!dst)
        return;
    uint8_t cr_a = VRAM_A_CR, cr_b = VRAM_B_CR, cr_c = VRAM_C_CR, cr_d = VRAM_D_CR;
    VRAM_A_CR = VRAM_ENABLE | VRAM_A_LCD;
    VRAM_B_CR = VRAM_ENABLE | VRAM_B_LCD;
    VRAM_C_CR = VRAM_ENABLE | VRAM_C_LCD;
    VRAM_D_CR = VRAM_ENABLE | VRAM_D_LCD;
    volatile uint16_t *d = dst;
    for (int i = 0; i < bytes / 2; i++)
        d[i] = src[2 * i] | src[2 * i + 1] << 8;
    VRAM_A_CR = cr_a;
    VRAM_B_CR = cr_b;
    VRAM_C_CR = cr_c;
    VRAM_D_CR = cr_d;
}

// One 256x256 texture for the camera masks, reserved before the enemy and
// effect textures: allocating it anew per camera (up to 64 KB) failed once
// texture memory got fragmented and left the masks white (room 102).
#define MASK_TEX_HEIGHT 256
static void reserve_mask_texture(void)
{
    uint8_t *zero = calloc(1, 256 * MASK_TEX_HEIGHT);
    static const uint16_t black[256];
    if (!zero)
        return;
    DC_FlushRange(zero, 256 * MASK_TEX_HEIGHT);
    glGenTextures(1, &mask_tex);
    glBindTexture(0, mask_tex);
    if (!glTexImage2D(0, 0, GL_RGB256, TEXTURE_SIZE_256, TEXTURE_SIZE_256, 0,
                      TEXGEN_TEXCOORD | GL_TEXTURE_COLOR0_TRANSPARENT, zero)) {
        glDeleteTextures(1, &mask_tex);
        mask_tex = -1;
    } else {
        DC_FlushRange(black, sizeof(black));
        glColorTableEXT(0, 0, 256, 0, 0, black);
    }
    free(zero);
}

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

// background, masks and the first 3D frame of camera cam
static void show_camera_view(int cam)
{
    char path[64];

    // Background: unpack into the hidden buffer; it is shown at the end,
    // together with the first 3D frame of the new camera.
    snprintf(path, sizeof(path), "nitro:/bg/%s_cam%02d.bin", bg_prefix, cam);
    BgHeader *bg = asset_load(path, NULL);
    if (!bg) {
        // a few rooms (e.g. 30C) repeat one camera and store a single image
        snprintf(path, sizeof(path), "nitro:/bg/%s_cam00.bin", bg_prefix);
        bg = asset_load(path, NULL);
    }
    if (!bg || bg->magic != BG_MAGIC)
        fatal(path);
    decompress(bg + 1, bg_ram, LZ77);

    // masks for this camera (optional), into the texture reserved at start
    if (have_masks)
        asset_free(masks.data);
    snprintf(path, sizeof(path), "nitro:/mask/%s_cam%02d.bin", bg_prefix, cam);
    have_masks = mask_tex >= 0 && masks_load(&masks, path);
    if (have_masks && masks.header->tex_height > MASK_TEX_HEIGHT) {
        asset_free(masks.data);
        have_masks = 0;
    }
    if (have_masks) {
        DC_FlushRange(masks.data, 512 + sizeof(MaskHeader) + 256 * masks.header->tex_height);
        glBindTexture(0, mask_tex);
        glColorSubTableEXT(0, 0, 256, 0, 0, masks.palette);
        texture_write(glGetTexturePointer(mask_tex), masks.pixels, 256 * masks.header->tex_height);
    }

    // The 3D layer shows what was flushed before the last vblank. Render the
    // new view now and flip the background in the same vblank, otherwise
    // Leon and the enemies stand at their old screen positions on the new
    // background until the next frame (or while the masks above load).
    render_frame();
    DC_FlushRange(bg_ram, BG_BYTES);
    swiWaitForVBlank();
    dmaCopy(bg_ram, BG_GFX, BG_BYTES); // well inside the vblank (~4.5 ms)
    for (int i = 1; i < 256; i++)
        BG_PALETTE[i] = bg->palette[i];
    asset_free(bg);
}

static void set_camera(int cam)
{
    script_camera_changed(cur_cam, cam);
    cur_cam = cam;
    show_camera_view(cam);
}

static void free_objects(void)
{
    for (int i = 0; i < obj_tex_count; i++)
        if (obj_tex_state[i] == 1)
            glDeleteTextures(1, &obj_tex[i]);
    if (obj_data)
        asset_free(obj_data);
    obj_data = NULL;
    obj_count = obj_tex_count = obj_tex_used = objects_ready = 0;
    memset(obj_tex_state, 0, sizeof(obj_tex_state));
    memset(objects, 0, sizeof(objects));
}

static int texture_size(int n) { int s = 0; while ((8 << s) < n) s++; return s; } // TEXTURE_SIZE_8 = 0

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void load_objects(int stage, int id)
{
    char path[48];
    uint32_t size;
    free_objects();
    snprintf(path, sizeof(path), "nitro:/room/room%d%02x0_obj.bin", stage, id);
    obj_data = asset_load(path, &size);
    if (!obj_data)
        return;
    if (size < 8 || rd32(obj_data) != OBJ_MAGIC) {
        asset_free(obj_data);
        obj_data = NULL;
        return;
    }
    obj_count = obj_data[4] | obj_data[5] << 8;
    int textures = obj_data[6] | obj_data[7] << 8;
    if (obj_count > MAX_OBJECTS)
        obj_count = MAX_OBJECTS;
    if (textures > MAX_OBJECTS)
        textures = MAX_OBJECTS;
    DC_FlushRange(obj_data, size);
    obj_tex_count = textures;
}

static const uint8_t *object_entry(int n) { return obj_data + 8 + 8 * obj_tex_count + 8 * n; }

static void upload_object_texture(int i)
{
    if (i >= obj_tex_count || obj_tex_state[i])
        return;
    const uint8_t *e = obj_data + 8 + 8 * i;
    int w = e[0] | e[1] << 8, h = e[2] | e[3] << 8;
    if (obj_tex_used + w * h + 512 > OBJ_TEX_BUDGET) {
        obj_tex_state[i] = 2;
        return;
    }
    const uint8_t *tex = obj_data + rd32(e + 4);
    const uint16_t *pal = (const uint16_t *)tex;
    glGenTextures(1, &obj_tex[i]);
    glBindTexture(0, obj_tex[i]);
    if (!glTexImage2D(0, 0, GL_RGB256, texture_size(w), texture_size(h), 0,
                      TEXGEN_TEXCOORD | (pal[0] ? 0 : GL_TEXTURE_COLOR0_TRANSPARENT), tex + 512)) {
        glDeleteTextures(1, &obj_tex[i]);
        obj_tex_state[i] = 2;
        return;
    }
    glColorTableEXT(0, 0, 256, 0, 0, pal);
    obj_tex_state[i] = 1;
    obj_tex_used += w * h + 512;
}

static int is_item_model(int n)
{
    for (int i = 0; i < NUM_AOTS; i++)
        if (aots[i].active && aots[i].type == SCE_ITEM && aots[i].data[6] == n)
            return 1;
    return 0;
}

// textures of the placed models: those of items first
static void upload_objects(void)
{
    if (!obj_data || !objects_ready)
        return;
    for (int pass = 0; pass < 2; pass++)
        for (int n = 0; n < obj_count; n++)
            if (objects[n].placed && !objects[n].hidden && is_item_model(n) == !pass)
                upload_object_texture(object_entry(n)[0] | object_entry(n)[1] << 8);
}

void game_object_set(const uint8_t *ins)
{
    int n = ins[1];
    if (n >= MAX_OBJECTS)
        return; // 255: no model
    objects[n].placed = 1;
    objects[n].type = ins[2];
    objects[n].flags = (ins[10] | ins[11] << 8) | 1;
    for (int k = 0; k < 3; k++) {
        objects[n].pos[k] = (int16_t)(ins[14 + 2 * k] | ins[15 + 2 * k] << 8);
        objects[n].dir[k] = (int16_t)(ins[20 + 2 * k] | ins[21 + 2 * k] << 8);
    }
    upload_objects(); // placed later by an event
}

void game_espr_on(const uint8_t *ins) { roomfx_on(ins); }
void game_espr_kill(int id, int type) { roomfx_kill(id, type); }

void game_floor_sound_set(int n, int on)
{
    if (n < room.header->num_flr) {
        RoomFloorSound *f = (RoomFloorSound *)&room.floor_sounds[n];
        f->floor = on ? f->floor & ~0x80 : f->floor | 0x80;
    }
}

void game_object_hide(int model)
{
    if (model < MAX_OBJECTS)
        objects[model].hidden = 1;
}

int game_object_member(int n, int member, int *value)
{
    if (n >= MAX_OBJECTS)
        return 0;
    if (member >= 0x0B && member <= 0x0D)
        *value = objects[n].pos[member - 0x0B];
    else if (member == 0x0F)
        *value = objects[n].dir[1];
    else
        return 0;
    return 1;
}

void game_object_member_set(int n, int member, int value)
{
    if (n >= MAX_OBJECTS)
        return;
    if (member >= 0x0B && member <= 0x0D)
        objects[n].pos[member - 0x0B] = (int16_t)value;
    else if (member == 0x0F)
        objects[n].dir[1] = (int16_t)value;
}

// FUN_8002bc00: (x, z) inside a camera's view area (the first switch zone
// whose cam0 is that camera), with the PS1's edge tests
static int in_view_area(int cam, int x, int z)
{
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if (s->cam0 != cam)
            continue;
        int64_t ax = x - s->x[0], az = z - s->z[0];
        int64_t x1 = s->x[1] - s->x[0], z1 = s->z[1] - s->z[0];
        int64_t x2 = s->x[2] - s->x[0], z2 = s->z[2] - s->z[0];
        int64_t x3 = s->x[3] - s->x[0], z3 = s->z[3] - s->z[0];
        return x1 * az <= z1 * ax && x3 * az >= z3 * ax && (z1 - z2) * (ax - x2) <= (x1 - x2) * (az - z2) &&
               (x3 - x2) * (az - z2) <= (z3 - z2) * (ax - x2);
    }
    return 1; // no zone for this camera
}

static void draw_objects(void)
{
    if (!obj_data)
        return;
    for (int n = 0; n < obj_count; n++) {
        const uint8_t *m = object_entry(n);
        uint32_t list = rd32(m + 4);
        int tex = m[0] | m[1] << 8;
        if (!objects[n].placed || objects[n].hidden || !list || tex >= obj_tex_count || obj_tex_state[tex] != 1)
            continue;
        // the PS1 (FUN_80033f70) draws a room object only while it stands in
        // the view area of the shown camera, unless flag 0x10 is set: things
        // behind a wall in another part of the room stay hidden
        if (objects[n].type != 1 && !(objects[n].flags & 0x10) &&
            !in_view_area(cur_cam, objects[n].pos[0], objects[n].pos[2]))
            continue;
        glPushMatrix();
        glTranslatef32(objects[n].pos[0], objects[n].pos[1], objects[n].pos[2]);
        glRotateXi(objects[n].dir[0] * 8);
        glRotateYi(objects[n].dir[1] * 8);
        glRotateZi(objects[n].dir[2] * 8);
        glBindTexture(0, obj_tex[tex]);
        glCallList((const u32 *)(obj_data + list));
        glPopMatrix(1);
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
    effect_clear();
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
    rbj_moves = rbj.data && rbj.header->num_anims > RBJ_LAND;
    for (int k = 0; rbj_moves && k < 4; k++)
        rbj_moves = rbj.anims[RBJ_CLIMB + k].count == rbj_move_keys[k];
    for (int i = 0; i < MAX_ENEMIES; i++) {
        enemy_set_room_set(i, NULL);
        if (rbj_enemy[i].data)
            asset_free(rbj_enemy[i].data);
        rbj_enemy[i].data = NULL;
        snprintf(path, sizeof(path), "nitro:/room/room%d%02x0_rbj_e%d.bin", stage, id, i);
        if (model_load(&rbj_enemy[i], path))
            enemy_set_room_set(i, &rbj_enemy[i]);
        else if (rbj_enemy[i].data)
            asset_free(rbj_enemy[i].data), rbj_enemy[i].data = NULL;
    }
    voice_stop();
    // path zones for the enemies (tools/blk2ds.py)
    enemy_set_zones(NULL);
    if (room_zones)
        asset_free(room_zones);
    snprintf(path, sizeof(path), "nitro:/room/room%d%02x0_blk.bin", stage, id);
    room_zones = asset_load(path, NULL);
    enemy_set_zones(room_zones);
    load_objects(stage, id);
    roomfx_load(stage, id);
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
static const RoomCamera *view_camera(void)
{
    return die.step >= DIE_ORBIT ? &die_cam : &room.cameras[cur_cam];
}

int camera_sees(int32_t x, int32_t y, int32_t z, int32_t margin)
{
    const RoomCamera *c = view_camera();
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

// Pose blending (FUN_80028a4c): every animation change of Leon (obj+0x14c =
// 0x70000 | anim, i.e. counter 7 in byte 0x14e) eases from the shown pose
// into the new one. Per game tick: shown = new + (shown - new) * counter / 8
// (angles the short way round, root offset too), then counter - 1.
#define POSE_BLEND_TICKS 7
#define POSE_MAX_BONES 32
static int32_t pose_rot[POSE_MAX_BONES][3];
static int32_t pose_ofs[3];
static const Model *pose_set;
static int pose_anim = -1;          // -1: next update snaps (new room / game)
static int pose_blend;

static void update_pose(void)
{
    const ModelAnim *a = &player.set->anims[player.anim];
    int key = player.key < a->count ? player.key : a->count - 1;
    int frame = ANIM_FRAME(player.set->anim_pool[a->first + key]);
    const ModelFrame *f = model_frame(player.set, frame);
    const int16_t *rot = model_frame_rot(player.set, frame);
    int bones = leon.header->num_bones < POSE_MAX_BONES ? leon.header->num_bones : POSE_MAX_BONES;
    if (pose_anim < 0)
        pose_blend = 0;
    else if (player.set != pose_set || player.anim != pose_anim)
        pose_blend = POSE_BLEND_TICKS;
    pose_set = player.set;
    pose_anim = player.anim;
    int w = pose_blend;               // eighths of the shown pose that stay
    for (int b = 0; b < bones; b++)
        for (int k = 0; k < 3; k++) {
            int32_t to = rot[b * 3 + k];
            int32_t d = ((pose_rot[b][k] - to + 16384) & 32767) - 16384;
            pose_rot[b][k] = (to + d * w / 8) & 32767;
        }
    for (int k = 0; k < 3; k++)
        pose_ofs[k] = f->offset[k] + (pose_ofs[k] - f->offset[k]) * w / 8;
    if (pose_blend)
        pose_blend--;
}

static void draw_bone(int bone)
{
    const ModelBone *b = &leon.bones[bone];
    const int32_t *rot = pose_rot[bone < POSE_MAX_BONES ? bone : 0];

    glPushMatrix();
    if (b->parent < 0)
        glTranslatef32(pose_ofs[0], pose_ofs[1], pose_ofs[2]);
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
        draw_bone(c);
    glPopMatrix(1);
}

static void draw_player(void)
{
    if (pose_anim < 0)
        update_pose();
    glPushMatrix();
    glTranslatef32(player.x, player.y, player.z);
    glRotateYi(player.dir);
    for (int b = 0; b < leon.header->num_bones; b++)
        if (leon.bones[b].parent < 0)
            draw_bone(b);
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

// RE2 models wind their faces so that (v1 - v0) x (v2 - v0) points into the
// model (95-98 % of the faces of Leon and the zombies; the rest are concave
// spots where that test is unreliable), and the PS1 skips faces turned away
// from the camera. The DS does the same with POLY_CULL_*, and culled polygons
// take no vertex or polygon RAM, so every model costs about half. Which mode
// keeps the faces turned toward the camera depends on the view setup, so a
// test triangle that faces the camera like a visible model face tells: the
// polygon RAM counter shows whether the hardware stored it.
int cull_models = POLY_CULL_NONE;   // used for Leon and the enemies
static int gx_vertices_last, gx_polygons_last; // whole frame, for the status line

static int stores_test_triangle(int mode)
{
    static const int32_t from[3] = { 0, -1000, -5000 }, to[3] = { 0, -1000, 0 };
    setup_camera(from, to, 290);
    while (GFX_BUSY)
        ;
    int before = GFX_POLYGON_RAM_USAGE;
    glPolyFmt(POLY_ALPHA(31) | mode);
    glBegin(GL_TRIANGLES);
    glVertex3v16(0, -1000, 0);
    glVertex3v16(0, -1100, 0);
    glVertex3v16(100, -1000, 0);
    glEnd();
    while (GFX_BUSY)
        ;
    return GFX_POLYGON_RAM_USAGE > before;
}

static void calibrate_culling(void)
{
    setBrightness(1, -16); // the test frame stays dark; the game fades in later
    int back = stores_test_triangle(POLY_CULL_BACK);
    int front = stores_test_triangle(POLY_CULL_FRONT);
    if (back != front)
        cull_models = back ? POLY_CULL_BACK : POLY_CULL_FRONT;
    // else: counters unusable, keep drawing both sides
    glFlush(0);
    swiWaitForVBlank();
}

// ---------------------------------------------------------------- death
// Leon's hip (bone 0): T(pos) * Ry(dir) * root offset
static void die_target(int32_t t[3])
{
    int32_t c = cosLerp(player.dir), s = sinLerp(player.dir);
    t[0] = player.x + ((c * pose_ofs[0] + s * pose_ofs[2]) >> 12);
    t[1] = player.y + pose_ofs[1];
    t[2] = player.z + ((-s * pose_ofs[0] + c * pose_ofs[2]) >> 12);
}

// FUN_80191200, once per tick from the orbit on
static void die_camera_tick(void)
{
    int32_t t[3];
    die_target(t);
    die.radius -= die.shrink;
    die.eye[1] -= die.rise;
    die.rise += die.rise_acc;
    die.eye[0] = t[0] + ((sinLerp(die.angle * 8) * die.radius) >> 12);
    die.eye[2] = t[2] - ((cosLerp(die.angle * 8) * die.radius) >> 12);
    die.angle = (die.angle - die.spin) & 0xFFF;
    die.at[0] += (t[0] - die.at[0]) / 30;
    die.at[2] += (t[2] - die.at[2]) / 30;
    die.at[1] += (t[1] - (die.at[1] + 300)) / 5;
    for (int k = 0; k < 3; k++) {
        die_cam.from[k] = die.eye[k];
        die_cam.to[k] = die.at[k];
    }
    die_cam.screen = DIE_SCREEN;
}

// FUN_80190644 with the zombie row of 0x801933ac: radius 3000 shrinking by
// 8, eye 1000 above the hip sinking by 5, turning by 2 from facing + 600
static void die_orbit_start(void)
{
    int32_t t[3];
    die_target(t);
    die.radius = 3000;
    die.shrink = 8;
    die.angle = (player.dir / 8 + 600) & 0xFFF;
    die.spin = 2;
    die.eye[1] = t[1] - 1000;
    die.rise = -5;
    die.rise_acc = 0;
    die.at[0] = t[0];
    die.at[1] = t[1] - 300;
    die.at[2] = t[2];
    die_camera_tick();
}

static void die_show_picture(void)
{
    BgHeader *bg = asset_load("nitro:/die/died00.bin", NULL);
    if (!bg || bg->magic != BG_MAGIC) {
        asset_free(bg);
        return;
    }
    decompress(bg + 1, bg_ram, LZ77);
    memcpy(die_palette, bg->palette, sizeof(die_palette));
    asset_free(bg);
    for (int i = 1; i < 256; i++)
        BG_PALETTE[i] = 0;
    DC_FlushRange(bg_ram, BG_BYTES);
    dmaCopy(bg_ram, BG_GFX, BG_BYTES);
    bgShow(bg_id);
}

// the picture's palette at level / 16
static void die_picture_level(int level)
{
    for (int i = 1; i < 256; i++) {
        uint16_t c = die_palette[i];
        int r = (c & 31) * level / 16, g = (c >> 5 & 31) * level / 16, b = (c >> 10 & 31) * level / 16;
        BG_PALETTE[i] = RGB15(r, g, b);
    }
}

static void start_game(void);

// one game tick of the death sequence
static void die_tick(void)
{
    if (die.step == DIE_OFF) {
        if (player.state != ST_DEAD)
            return;
        memset(&die, 0, sizeof(die));
        die.step = DIE_WAIT;
        die.ambient = 60;
        die.light[0] = die.light[1] = die.light[2] = 195;
        voice_play(0, 0);
    }
    die.t++;
    switch (die.step) {
    case DIE_WAIT:
        if (die.t > DIE_DELAY) {
            die.step = DIE_FLASH;
            die.flash = 0x400;
            die.t = 0;
        }
        break;
    case DIE_FLASH: // 0x80193706 += v, v += a, a += 24 until it saturates
        die.flash_v += die.flash_a;
        die.flash_a += 24;
        die.flash += die.flash_v;
        if (die.flash >= 32767 && voice_playing()) { // hold white until the clip ends
            die.flash = 32766;
            die.flash_v = 1;
            die.flash_a = 0;
        }
        if (die.flash >= 32767) {
            die.step = DIE_ORBIT;
            die.t = 0;
            bgHide(bg_id);
            die_orbit_start();
            setBrightness(1, 0);
        } else {
            setBrightness(1, die.flash * 16 / 32767);
        }
        break;
    case DIE_ORBIT:
        die_camera_tick();
        if (die.t > DIE_ORBIT_TICKS) {
            die.step = DIE_PICTURE;
            die.t = 0;
            die.shrink = die.spin = die.rise = 0;
            die_show_picture();
        }
        break;
    case DIE_PICTURE:
        if (die.t <= 32)
            die_picture_level(die.t / 2);
        if (die.t >= 6 && die.t < 10) { // red flash, then towards grey
            die.light[0] = 255;
            die.light[1] = die.light[2] = 0;
        } else if (die.t >= 10 && die.t <= 20) {
            die.light[0] -= 12;
            die.light[1] += 13;
            die.light[2] += 13;
        }
        die_camera_tick();
        if (die.t >= 170) {
            die.step = DIE_FADE;
            die.t = 0;
            die.rise = 10;
            die.rise_acc = 2;
            die.spin = 16;
        }
        break;
    case DIE_FADE:
        die_camera_tick();
        if (die.ambient > 0)
            die.ambient--;
        for (int k = 0; k < 3; k++)
            die.light[k] = die.light[k] > 4 ? die.light[k] - 4 : 0;
        if (die.t >= 110)
            setBrightness(1, -16 * (die.t - 110) / 53);
        if (die.t >= 163) {
            setBrightness(1, -16);
            die.step = DIE_OFF;
            bgShow(bg_id);
            start_game();
        }
        break;
    }
}

// the vertex colour models are lit with (textures are modulated by it)
static void model_colour(void)
{
    if (die.step < DIE_PICTURE) {
        glColor3b(255, 255, 255);
        return;
    }
    int c[3];
    for (int k = 0; k < 3; k++) {
        c[k] = die.ambient + die.light[k];
        if (c[k] > 255)
            c[k] = 255;
    }
    glColor3b(c[0], c[1], c[2]);
}

static void render_frame(void)
{
    const RoomCamera *cam = view_camera();
    setup_projection(cam);
    // Textures are modulated with the vertex colour: white but for the death.
    model_colour();
    glPolyFmt(POLY_ALPHA(31) | cull_models | POLY_ID(1));
    draw_player();
    draw_objects();
    update_occlusion();
    int fx = effect_count() + roomfx_count();
    enemy_draw((have_masks ? 4 * masks.header->num_quads : 0) + 4 * fx,
               (have_masks ? masks.header->num_quads : 0) + fx);
    effect_draw(cam->from, cam->to);
    roomfx_draw(cam->from, cam->to);
    glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE | POLY_ID(2));
    if (die.step < DIE_ORBIT)
        draw_masks();
    while (GFX_BUSY)
        ;
    gx_vertices_last = GFX_VERTEX_RAM_USAGE;
    gx_polygons_last = GFX_POLYGON_RAM_USAGE;
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
        music_frame();
        voice_frame();
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
// Shapes as the PS1 tests them (OpenBiohazard2 world/collision.go): 6 = a
// circle of diameter w, 7 / 8 = ellipses filling the box (long axis x / z;
// e.g. the low wall in front of the stairs of room 103, which as a rectangle
// covered the top step). Triangles are still tested as their box.
static int32_t penetration(const RoomBox *b, int32_t x, int32_t z)
{
    if (b->shape == 6 || b->shape == 7 || b->shape == 8) {
        float ra = (b->shape == 6 ? b->w : b->w) * 0.5f, rb = (b->shape == 6 ? b->w : b->d) * 0.5f;
        float dx = x - (b->x + ra), dz = z - (b->z + rb);
        ra += PLAYER_RADIUS;
        rb += PLAYER_RADIUS;
        float q = dx * dx / (ra * ra) + dz * dz / (rb * rb);
        if (q >= 1.0f)
            return 0;
        int32_t depth = (int32_t)((1.0f - sqrtf(q)) * (ra < rb ? ra : rb));
        return depth > 0 ? depth : 1;
    }
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
// mask: BOX_PLAYER for Leon, BOX_ENEMY for the enemies.
static int blocked_for(uint16_t mask, int32_t from_x, int32_t from_z, int32_t x, int32_t z)
{
    uint32_t floor_bit = 1u << player_floor();
    for (int i = 0; i < room.header->num_boxes; i++) {
        const RoomBox *b = &room.boxes[i];
        if (!(b->floors & floor_bit) || is_slope(b) || !(b->flag & mask))
            continue;
        int32_t depth = penetration(b, x, z);
        if (depth > 0 && depth >= penetration(b, from_x, from_z))
            return 1;
    }
    return 0;
}

static int blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z)
{
    return blocked_for(BOX_PLAYER, from_x, from_z, x, z);
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
    if (!cut_auto_enabled() || die.step >= DIE_ORBIT)
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
    pose_anim = -1;
    cam = cut_replace(cam);
    set_camera(cam < room.header->num_cameras ? cam : 0);
}

// ---------------------------------------------------------------- script hooks
void game_show_message(const char *text)
{
    message_text = text;
    menu_message(text);
}

int game_message_open(void) { return menu_message_open(); }

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

void game_voice(int kind, int n)
{
    voice_play(kind == 0 ? cur_stage : 0, n); // kind 1: the common table (VOICE00)
}

int game_voice_playing(void) { return voice_playing(); }

int game_player_member(int member, int *value)
{
    switch (member) {
    case 0x0B: *value = player.x; return 1;
    case 0x0C: *value = player.y; return 1;
    case 0x0D: *value = player.z; return 1;
    case 0x0F: *value = (player.dir / 8) & 0xFFF; return 1;
    }
    return 0;
}

void game_player_member_set(int member, int value)
{
    switch (member) {
    case 0x0B: player.x = (int16_t)value; break;
    case 0x0C: player.y = (int16_t)value; break;
    case 0x0D: player.z = (int16_t)value; break;
    case 0x0F: player.dir = ((int16_t)value * 8) & 32767; break;
    }
}

// Leon as plc.c moves him: the room's set (mode 0) and his weapon set
// (walking = move 0, standing = move 2: 0x800116bc / 0x800116c8)
static PlcBody player_body(void)
{
    PlcBody b = { .x = player.x, .z = player.z, .dir = player.dir, .set = player.set,
                  .anim = player.anim, .key = player.key, .room_set = rbj.data ? &rbj : NULL,
                  .own_set = &leon, .own_first = PLW_FIRST, .walk_anim = PLW_FIRST + 0,
                  .idle_anim = PLW_FIRST + 2, .table = 16 };
    return b;
}

static void player_body_back(const PlcBody *b)
{
    player.x = b->x;
    player.z = b->z;
    player.dir = b->dir;
    player.set = b->set;
    player.anim = b->anim;
    player.key = b->key;
}

void game_plc(int work_type, int index, const uint8_t *ins)
{
    if (work_type == 3) {
        enemy_plc(index, ins);
        return;
    }
    if (work_type != 1 || player.state >= ST_GRABBED)
        return;
    PlcBody b = player_body();
    plc_command(&player.plc, &b, ins);
    player_body_back(&b);
    if (player.plc.active) {
        player.state = ST_SCRIPT;
    } else if (player.state == ST_SCRIPT) {
        player.state = ST_IDLE; // plc_ret
        set_anim(ANIM_IDLE);
    }
}

// one game tick of a scripted Leon
static void script_move_tick(void)
{
    PlcBody b = player_body();
    plc_tick(&player.plc, &b);
    player_body_back(&b);
    if (b.footstep)
        sound_play(room_bank, floor_sound(), 100, 64);
    update_height();
    check_camera_switch();
}

void game_spawn_enemy(int slot, int type, int flags, int voice, int x, int y, int z, int dir_re2)
{
    enemy_spawn(slot, type, flags, voice, x, y, z, dir_re2);
}

// Enemy sounds get quieter with the distance to Leon (the PS1 places them in
// 3D; this is the simple version).
void game_enemy_sound(int sound, int32_t x, int32_t z)
{
    float dx = (float)(x - player.x), dz = (float)(z - player.z);
    int d = (int)sqrtf(dx * dx + dz * dz);
    int volume = d >= 20000 ? 20 : 127 - d * 107 / 20000;
    sound_play(enemy_bank, sound, volume, 64);
}

void game_player_voice(int sound)
{
    sound_play(voice_bank, sound, 127, 64);
}

// FUN_8003cf38: bites under 41 count 1.5 times; at the first drop below 0 by
// at most 14 Leon is left with 0 HP and tears free instead of dying.
int game_player_damage(int damage)
{
    if (damage < 41)
        damage += damage >> 1;
    player.hp -= damage;
    if (player.hp >= 0)
        return 0;
    if (player.hp < -14 || player.saved_once)
        return 2;
    player.hp = 0;
    player.saved_once = 1;
    return 1;
}

void game_player_grabbed(int on)
{
    if (on) {
        player.state = ST_GRABBED;
        player.slope = -1;
    } else {
        player.state = ST_IDLE;
        set_anim(ANIM_IDLE);
    }
}

void game_player_pose(const Model *set, int anim, int key, int32_t x, int32_t z, int16_t dir)
{
    if (player.hp < 0)
        player.state = ST_DEAD;
    player.set = set;
    player.anim = anim;
    player.key = key;
    player.x = x;
    player.z = z;
    player.dir = dir;
    check_camera_switch();
}

void game_player_died(void)
{
    // the death sequence (die_tick) started when Leon was killed
}

void game_bgm_control(int id, int op, int type, int left, int right)
{
    music_control(id, op, type, left, right);
}

// movie_on (FUN_80031494): the movie table 0x8009a578, field 12 = the STR
// file. Entries 0-2 are the logo and the opening, 15 Claire's opening; 16
// another cut of the ending. The in-game movies are converted at 10 fps
// (tools/build_assets.sh). Afterwards the room picture comes back.
static const char *const movie_names[] = {
    [3] = "r10b", [4] = "r200", [5] = "r505", [6] = "r505", [7] = "r108l", [8] = "r109",
    [9] = "r204l", [10] = "r409", [11] = "r700l", [12] = "r703l", [13] = "r704l",
    [14] = "opn2", [16] = "r704l",
};

void game_movie(int n)
{
    if (n < 0 || n >= (int)(sizeof(movie_names) / sizeof(movie_names[0])) || !movie_names[n])
        return;
    movie_play(movie_names[n]);
    movie_end();
    if (cur_cam >= 0)
        show_camera_view(cur_cam);
    setBrightness(1, 0);
    game_tick_acc = 0; // no catching up on the ticks the movie took
}

void game_bgmtbl_set(int stage, int room, int main, int sub)
{
    music_table_set(stage, room, main, sub);
}

// status screen sounds (menu.c): CORE00, as the PS1's bank 4
void game_menu_sound(int sound)
{
    sound_play(voice_bank, sound, 127, 64);
}

void game_blood(int kind, int32_t x, int32_t y, int32_t z, int16_t dir, int speed, int32_t floor_y)
{
    effect_blood(kind, x, y, z, dir, speed, floor_y);
}

int game_enemy_member(int slot, int member, int *value)
{
    return enemy_member_get(slot, member, value);
}

void game_enemy_member_set(int slot, int member, int value)
{
    enemy_member_set(slot, member, value);
}

// Does the segment p0-p1 pass through the box lo..hi? (slab test)
static int segment_hits_box(const float p0[3], const float p1[3], const float lo[3], const float hi[3])
{
    float t0 = 0.0f, t1 = 1.0f;
    for (int k = 0; k < 3; k++) {
        float d = p1[k] - p0[k];
        if (fabsf(d) < 1e-3f) {
            if (p0[k] < lo[k] || p0[k] > hi[k])
                return 0;
            continue;
        }
        float a = (lo[k] - p0[k]) / d, b = (hi[k] - p0[k]) / d;
        if (a > b) {
            float tmp = a;
            a = b;
            b = tmp;
        }
        if (a > t0)
            t0 = a;
        if (b < t1)
            t1 = b;
        if (t0 > t1)
            return 0;
    }
    return 1;
}

// Line of sight for enemies (0x8004d000): room boxes with flag 0x2000 block
// it, except shapes 1-5 and 13 (table 0x8009f480); the others count as their
// rectangle. A box reaches from its lowest floor up to the height in its type
// field, so low ones like cars do not hide Leon from a zombie's eyes.
// Does box i (with all bits of need in its flag) block the line p0-p1?
static int sight_box_hit_flags(int i, const float p0[3], const float p1[3], uint16_t need)
{
    static const uint8_t skip_shape[16] = { 0, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0 };
    const RoomBox *b = &room.boxes[i];
    if (!(b->flag & 0x2000) || (b->flag & need) != need || skip_shape[b->flag & 15] || !b->floors)
        return 0;
    int32_t top = -((b->type >> 11) * 100 + ((b->type >> 6) & 31) * 1800);
    int32_t bottom = 0;
    for (uint32_t f = b->floors; !(f & 1); f >>= 1)
        bottom += FLOOR_HEIGHT;
    const float lo[3] = { (float)b->x, (float)top, (float)b->z };
    const float hi[3] = { (float)(b->x + b->w), (float)bottom, (float)(b->z + b->d) };
    return segment_hits_box(p0, p1, lo, hi);
}

static int sight_box_hit(int i, const float p0[3], const float p1[3])
{
    return sight_box_hit_flags(i, p0, p1, 0);
}

int game_sight_blocked(int32_t x0, int32_t y0, int32_t z0, int32_t x1, int32_t y1, int32_t z1)
{
    const float p0[3] = { (float)x0, (float)y0, (float)z0 }, p1[3] = { (float)x1, (float)y1, (float)z1 };
    for (int i = 0; i < room.header->num_boxes; i++)
        if (sight_box_hit(i, p0, p1))
            return 1;
    return 0;
}

// Enemies behind walls. The backgrounds' masks only cover what the PS1
// artists expected in front, and zombies pressing against a wall from the
// other side can stand nearer than a wall's mask (room 102, camera 1: the
// courtyard zombies at the gate, masks at depth 9920, zombies at ~9000).
// Hidden: an enemy the camera would see only through a tall sight-blocking wall
// that does not also stand between the camera and Leon (many cameras look
// through the room's outer walls; those walls then count for nobody). Only
// solid walls count, that collide with everything (flag 0xFC00): a gate
// that only some objects collide with (room 102 box 0: 0xF400, a chain-link
// fence) is see-through.
#define MAX_SIGHT_BOXES 64
#define SOLID_WALL 0xFC00
#define TALL_WALL 3600      // counters and shelves (1800) never hide a whole person

// an enemy inside a wall box (room 101: zombies left in the shop window when
// the script closes it again) is not behind it
static int stands_in(int i, int32_t x, int32_t z)
{
    const RoomBox *b = &room.boxes[i];
    return x > b->x - 400 && x < b->x + b->w + 400 && z > b->z - 400 && z < b->z + b->d + 400;
}

static int tall_wall(int i)
{
    const RoomBox *b = &room.boxes[i];
    return (b->type >> 11) * 100 + ((b->type >> 6) & 31) * 1800 >= TALL_WALL;
}
static uint64_t walls_before_leon;  // set per frame by update_occlusion()

static void update_occlusion(void)
{
    const RoomCamera *c = &room.cameras[cur_cam];
    const float eye[3] = { (float)c->from[0], (float)c->from[1], (float)c->from[2] };
    walls_before_leon = 0;
    for (int h = 0; h < 2; h++) {
        const float leon[3] = { (float)player.x, (float)(player.y - (h ? 1700 : 300)), (float)player.z };
        for (int i = 0; i < room.header->num_boxes && i < MAX_SIGHT_BOXES; i++)
            if (tall_wall(i) && sight_box_hit_flags(i, eye, leon, SOLID_WALL))
                walls_before_leon |= 1ull << i;
    }
}

int camera_occluded(int32_t x, int32_t y, int32_t z)
{
    if (die.step >= DIE_ORBIT)
        return 0; // the free camera is not where the room's walls were sorted for
    const RoomCamera *c = &room.cameras[cur_cam];
    const float eye[3] = { (float)c->from[0], (float)c->from[1], (float)c->from[2] };
    for (int h = 0; h < 2; h++) { // hidden only if feet and head (2000 up: above 1800-high boxes) are
        const float p[3] = { (float)x, (float)(y - (h ? 2000 : 100)), (float)z };
        int hit = 0;
        for (int i = 0; i < room.header->num_boxes && i < MAX_SIGHT_BOXES && !hit; i++)
            hit = !(walls_before_leon >> i & 1) && tall_wall(i) && !stands_in(i, x, z) &&
                  sight_box_hit_flags(i, eye, p, SOLID_WALL);
        if (!hit)
            return 0;
    }
    return 1;
}

// room collision for enemies (same rule as the player, on the floor of y)
int game_blocked(int32_t from_x, int32_t from_z, int32_t x, int32_t z, int32_t y)
{
    int32_t saved = player.y;
    player.y = y; // player_floor() reads it
    int b = blocked_for(BOX_ENEMY, from_x, from_z, x, z);
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

// NitroFS (the game data inside the ROM). calico's nitroFSInit reads the file
// argv[0] names (flashcart menus pass it) or, without argv, the cartridge. With
// a Slot-1 DLDI driver the ARM7 owns the card bus, so the ARM9 cannot read the
// cartridge: melonDS with DLDI passes no argv and fails both ways. Then look for
// the ROM on the SD card itself (start.command copies it there for melonDS).
static NitroRom sd_rom;
static int nitro_step; // how far nitro_init got, for the error screen

// Reading the ROM file: FAT seeks forward from the current cluster but walks
// the cluster chain from the start to go back, and the movie (picture and
// sound) or the music and a room read from far apart places in turn. So keep
// a few handles and use the one already just before the offset: each stream
// then reads forward through its own handle.
#define SD_HANDLES 3
#define SD_BUFFER 16384
static struct {
    FILE *f;
    u32 pos, used;
} sd_files[SD_HANDLES];
static const char *sd_rom_path;
static u32 sd_clock;

static bool sd_rom_read(void *user, u32 offset, void *buf, u32 size)
{
    (void)user;
    int pick = -1;
    for (int i = 0; i < SD_HANDLES; i++) // the closest one at or before offset
        if (sd_files[i].f && sd_files[i].pos <= offset && (pick < 0 || sd_files[i].pos > sd_files[pick].pos))
            pick = i;
    if (pick < 0) { // none: an unused or the least recently used one
        pick = 0;
        for (int i = 1; i < SD_HANDLES; i++)
            if (!sd_files[i].f || (sd_files[pick].f && sd_files[i].used < sd_files[pick].used))
                pick = i;
        if (!sd_files[pick].f) {
            sd_files[pick].f = fopen(sd_rom_path, "rb");
            if (!sd_files[pick].f)
                return false;
            setvbuf(sd_files[pick].f, NULL, _IOFBF, SD_BUFFER);
            sd_files[pick].pos = 0;
        }
    }
    FILE *f = sd_files[pick].f;
    if (sd_files[pick].pos != offset && fseek(f, offset, SEEK_SET) != 0)
        return false;
    size_t n = fread(buf, 1, size, f);
    sd_files[pick].pos = offset + n;
    sd_files[pick].used = ++sd_clock;
    return n == size;
}

static void sd_rom_close(void *user)
{
    (void)user;
    for (int i = 0; i < SD_HANDLES; i++)
        if (sd_files[i].f) {
            fclose(sd_files[i].f);
            sd_files[i].f = NULL;
        }
}

static const NitroRomIface sd_rom_iface = { sd_rom_read, sd_rom_close };

static bool nitro_init(void)
{
    static const char *const paths[] = { "fat:/re2ds.nds", "sd:/re2ds.nds" };
    nitro_step = 1;
    if (nitroFSInit(NULL)) // also mounts the SD card (fatInitDefault)
        return true;
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f)
            continue;
        // only this very build: another one's files lie elsewhere in the ROM
        static uint8_t header[0x160];
        if (fread(header, 1, sizeof(header), f) != sizeof(header) ||
            memcmp(header, g_envAppNdsHeader, sizeof(header)) != 0) {
            fclose(f);
            nitro_step = 4;
            continue;
        }
        setvbuf(f, NULL, _IOFBF, SD_BUFFER);
        sd_files[0].f = f;
        sd_files[0].pos = sizeof(header);
        sd_rom_path = paths[i];
        nitro_step = 2;
        NitroRomParams params = {
            .fat_offset = g_envAppNdsHeader->fat_rom_offset,
            .fat_sz = g_envAppNdsHeader->fat_size,
            .fnt_offset = g_envAppNdsHeader->fnt_rom_offset,
            .fnt_sz = g_envAppNdsHeader->fnt_size,
            .img_offset = 0,
        };
        if (nitroromOpen(&sd_rom, &params, &sd_rom_iface, NULL)) {
            nitro_step = 3;
            return nitroFSMount(&sd_rom);
        }
        sd_rom_close(NULL);
    }
    return false;
}

// The init script may test the arrival camera (VAR_CUT), e.g. room 107.
static void enter_room(int stage, int id, int arrival_cam)
{
    load_room(stage, id);
    music_room(stage, id);
    script_camera_changed(-1, arrival_cam);
    script_room_enter(&room);

    // the enemy voices of the room pick its enemy sound set
    char path[40];
    int set = enemy_sound_set(enemy_pairs);
    sound_bank_free(enemy_bank);
    enemy_bank = NULL;
    if (set >= 0) {
        snprintf(path, sizeof(path), "nitro:/sound/enem%02d.bin", set);
        enemy_bank = sound_bank_load(path);
    }
}

// New game, and after Leon died (the PS1 shows "YOU DIED" and goes back to
// the title; here the game simply starts over)
static void new_game(void)
{
    memset(&die, 0, sizeof(die));
    script_state_reset();
    music_table_reset();
    memset(lock_open, 0, sizeof(lock_open));
    save_count = 0;
    player.state = ST_IDLE;
    player.hp = PLAYER_HP;
    player.saved_once = 0;
    // Leon starts with the knife and the H&K VP70 (rounds: unverified)
    inventory_reset();
    memset(item_box, 0, sizeof(item_box));
    files_found = 0;
    inventory_add(0x01, 0);
    inventory_add(0x02, 18);
    inventory_equipped = 1;
    player.aim_target = -1;
    enter_room(START_STAGE, START_ROOM, -1);
    if (!room.data)
        fatal("room/room1000.bin missing");
    place_player(START_X, 0, START_Z, START_DIR, START_CAM);
    objects_ready = 1;
    upload_objects();
    fade(-1);
}

// ---------------------------------------------------------------- saving
// A frame while a choice is open on the lower screen
static void choice_frame(void)
{
    if (room.data)
        render_frame();
    swiWaitForVBlank();
    music_frame();
    voice_frame();
}

// ---------------------------------------------------------------- documents
// The pages of a document (tools/files2ds.py: files/fileNN_P.bin, page 0 the
// title) on the upper screen in place of the room picture: A / right / touch
// next page, left / L back, B / START closes (or A on the last page).
static int file_pages(int k)
{
    static uint8_t *index;
    if (!index)
        index = asset_load("nitro:/files/index.bin", NULL);
    return index && k >= 0 && k < index[0] ? index[1 + k] : 0;
}

static void show_file_page(int k, int page)
{
    char path[40];
    snprintf(path, sizeof(path), "nitro:/files/file%02d_%d.bin", k, page);
    BgHeader *bg = asset_load(path, NULL);
    if (!bg || bg->magic != BG_MAGIC) {
        asset_free(bg);
        return;
    }
    decompress(bg + 1, bg_ram, LZ77);
    DC_FlushRange(bg_ram, BG_BYTES);
    swiWaitForVBlank();
    dmaCopy(bg_ram, BG_GFX, BG_BYTES);
    for (int i = 1; i < 256; i++)
        BG_PALETTE[i] = bg->palette[i];
    asset_free(bg);
}

static void show_file(int k)
{
    int pages = file_pages(k);
    if (!pages)
        return;
    uint32_t layers = REG_DISPCNT & (DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE);
    REG_DISPCNT &= ~(DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE); // no 3D, no PAUSE
    int page = 0, shown = -1;
    for (;;) {
        if (page != shown) {
            show_file_page(k, page);
            shown = page;
        }
        scanKeys();
        uint32_t down = keysDown();
        if (down & (KEY_A | KEY_RIGHT | KEY_TOUCH)) {
            if (page + 1 >= pages)
                break;
            page++;
            game_menu_sound(4);
        } else if ((down & (KEY_LEFT | KEY_L)) && page > 0) {
            page--;
            game_menu_sound(4);
        } else if (down & (KEY_B | KEY_START)) {
            break;
        }
        swiWaitForVBlank();
        music_frame();
        voice_frame();
    }
    REG_DISPCNT |= layers;
    if (cur_cam >= 0)
        show_camera_view(cur_cam); // the room picture again
}

// L on the status screen: the documents found so far
static void open_files(void)
{
    const char *lines[NUM_FILES];
    int which[NUM_FILES], n = 0;
    for (int k = 0; k < NUM_FILES; k++)
        if (files_found & (1u << k)) {
            lines[n] = item_name(FIRST_FILE + k);
            which[n++] = k;
        }
    if (!n) {
        game_show_message("There are no files yet.");
        return;
    }
    int c = menu_choose("Files", lines, n, 0, 1, choice_frame);
    if (c >= 0)
        show_file(which[c]);
}

static int any_save(void)
{
    if (!save_read(saves))
        return 0;
    for (int i = 0; i < SAVE_SLOTS; i++)
        if (saves[i].used)
            return 1;
    return 0;
}

// the lines of the slot list; empty slots only when saving
static int pick_slot(const char *title, int saving)
{
    static char text[SAVE_SLOTS][40];
    const char *lines[SAVE_SLOTS];
    for (int i = 0; i < SAVE_SLOTS; i++) {
        const SaveSlot *s = &saves[i];
        if (s->used)
            snprintf(text[i], sizeof(text[i]), "%2d LEON  ROOM %d%02X  SAVE %u", i + 1, s->stage, s->room,
                     (unsigned)s->saves); // at most 28 characters
        else
            snprintf(text[i], sizeof(text[i]), "%2d --- NO DATA ---", i + 1);
        lines[i] = text[i];
    }
    for (;;) {
        int n = menu_choose(title, lines, SAVE_SLOTS, last_slot, 1, choice_frame);
        if (n < 0 || saving || saves[n].used)
            return n;
    }
}

// a typewriter (AOT type 9): uses up an ink ribbon to save
#define ITEM_INK_RIBBON 0x1E
static void use_typewriter(void)
{
    int ribbon = -1;
    for (int i = 0; i < INV_SLOTS; i++)
        if (inventory[i].item == ITEM_INK_RIBBON && inventory[i].count)
            ribbon = i;
    int debug = ribbon < 0 && (keysHeld() & KEY_L); // debug: L + A saves without a ribbon
    if (ribbon < 0 && !debug) {
        game_show_message("It's an old typewriter. If I had an Ink Ribbon, I could save my progress...");
        return;
    }
    static const char *const yes_no[] = { "YES", "NO" };
    if (menu_choose("It's an old typewriter. You can save your progress with this. Will you use the "
                    "Ink Ribbon?", yes_no, 2, 0, 1, choice_frame) != 0)
        return;
    if (!save_available()) {
        game_show_message("There is no SD card: the game cannot be saved. (melonDS: enable DLDI in the emulator settings.)");
        return;
    }
    save_read(saves);
    int n = pick_slot("Save to which file?", 1);
    if (n < 0)
        return;
    SaveSlot *s = &saves[n];
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->stage = cur_stage;
    s->room = cur_room;
    s->cam = cur_cam;
    s->saves = ++save_count;
    s->hp = player.hp;
    s->x = player.x;
    s->y = player.y;
    s->z = player.z;
    s->dir = player.dir;
    s->equipped = inventory_equipped;
    s->saved_once = player.saved_once;
    if (ribbon >= 0 && --inventory[ribbon].count == 0) // the ribbon used is gone before the state is kept
        inventory[ribbon].item = 0;
    memcpy(s->inventory, inventory, sizeof(s->inventory));
    memcpy(s->box, item_box, sizeof(s->box));
    s->files = files_found;
    memcpy(s->locks, lock_open, sizeof(s->locks));
    script_state_get(s->script);
    s->bgm_saved = music_table_get(s->bgm, sizeof(s->bgm)) > 0;
    last_slot = n;
    if (!save_write(saves)) {
        if (ribbon >= 0) { // nothing saved: the ribbon is kept
            inventory[ribbon].item = ITEM_INK_RIBBON;
            inventory[ribbon].count++;
        }
        save_count--;
        game_show_message("The game could not be saved.");
        return;
    }
    game_show_message("Your progress was saved.");
}

static void load_game(int n)
{
    const SaveSlot *s = &saves[n];
    memset(&die, 0, sizeof(die));
    script_state_set(s->script);
    music_table_reset();
    if (s->bgm_saved) // saves from before version 2 keep the starting table
        music_table_put(s->bgm, sizeof(s->bgm));
    memcpy(lock_open, s->locks, sizeof(lock_open));
    memcpy(inventory, s->inventory, sizeof(inventory));
    memcpy(item_box, s->box, sizeof(item_box));
    files_found = s->files;
    inventory_equipped = s->equipped;
    save_count = s->saves;
    last_slot = n;
    player.state = ST_IDLE;
    player.hp = s->hp;
    player.saved_once = s->saved_once;
    player.aim_target = -1;
    enter_room(s->stage, s->room, -1);
    if (!room.data)
        fatal("the saved room is missing");
    place_player(s->x, s->y, s->z, s->dir, s->cam);
    objects_ready = 1;
    upload_objects();
    fade(-1);
}

// At the start and after Leon died: a new game, or one from re2ds.sav
// The PS1 title screen (tools/title2ds.py: title/press.bin, new.bin,
// load.bin) on the upper screen: PRESS ANY BUTTON, then NEW GAME / LOAD GAME
// with up / down, A or START to choose. LOAD GAME without saves stays.
static void show_title(const char *name)
{
    char path[32];
    snprintf(path, sizeof(path), "nitro:/title/%s.bin", name);
    BgHeader *bg = asset_load(path, NULL);
    if (!bg || bg->magic != BG_MAGIC) {
        asset_free(bg);
        return;
    }
    decompress(bg + 1, bg_ram, LZ77);
    DC_FlushRange(bg_ram, BG_BYTES);
    swiWaitForVBlank();
    dmaCopy(bg_ram, BG_GFX, BG_BYTES);
    for (int i = 1; i < 256; i++)
        BG_PALETTE[i] = bg->palette[i];
    asset_free(bg);
}

static int skip_movies;               // SELECT held at start: no title movie, no opening

static int title_screen(void)
{
    uint32_t layers = REG_DISPCNT & (DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE);
    REG_DISPCNT &= ~(DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE);
    bgShow(bg_id);
    show_title("press");
    setBrightness(1, 0);
    setBrightness(2, -16); // the lower screen stays black (it holds leftovers until the status screen draws)
    for (;;) { // PRESS ANY BUTTON
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & (KEY_A | KEY_B | KEY_START | KEY_X | KEY_Y | KEY_TOUCH))
            break;
    }
    game_menu_sound(5);
    int sel = any_save() ? 1 : 0, shown = -1;
    for (;;) {
        if (sel != shown) {
            show_title(sel ? "load" : "new");
            shown = sel;
        }
        swiWaitForVBlank();
        scanKeys();
        uint32_t down = keysDown();
        if (down & (KEY_UP | KEY_DOWN)) {
            sel ^= 1;
            game_menu_sound(4);
        } else if (down & (KEY_A | KEY_START)) {
            if (sel == 1 && !any_save()) {
                game_menu_sound(7); // nothing to load
                continue;
            }
            game_menu_sound(5);
            break;
        }
    }
    setBrightness(1, -16); // the room fades in as after the movie
    setBrightness(2, 0);
    REG_DISPCNT |= layers;
    return sel;
}

static void start_game(void)
{
    for (;;) {
        if (title_screen() == 0)
            break;
        int n = pick_slot("Load which file?", 0);
        if (n >= 0) {
            load_game(n);
            return;
        }
    }
    // NEW GAME: the opening (Leon meets Claire, the tanker crash) first
    if (!skip_movies && !movie_play("opn1"))
        movie_play("opn2");
    movie_end();
    setBrightness(1, -16);
    new_game();
}

static void warp_to(int index)
{
    const RoomRef *r = &room_index[index];
    fade(1);
    enter_room(r->stage, r->room, -1);
    const RoomHeader *h = room.header;
    place_player(h->spawn[0], h->spawn[1], h->spawn[2], h->spawn_dir, h->start_camera);
    objects_ready = 1;
    upload_objects();
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
        if (keysHeld() & KEY_L) {
            lock_open[key_id & 0x7F] = 1; // debug: L + A opens every lock
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
        while (door_frame()) {
            swiWaitForVBlank();
            music_frame();
            voice_frame();
        }
        door_end();
        setBrightness(1, -16);
        bgShow(bg_id);
    }
    objects_ready = 1; // after the door: its texture needs room too
    upload_objects();
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
        if (item >= FIRST_FILE && item < FIRST_FILE + NUM_FILES) {
            // a document: read at once, then kept in the files (L on the status screen)
            files_found |= 1u << (item - FIRST_FILE);
            flag_set(FLAGS_ITEM_PICKED, picked, 1);
            a->active = 0;
            game_object_hide(a->data[6]);
            show_file(item - FIRST_FILE);
            snprintf(text, sizeof(text), "You found the %s. (Status screen: L)", item_name(item));
            game_show_message(text);
            break;
        }
        if (!inventory_add(item, amount)) {
            game_show_message("You cannot carry any more.");
            break;
        }
        flag_set(FLAGS_ITEM_PICKED, picked, 1);
        a->active = 0;
        game_object_hide(a->data[6]);
        snprintf(text, sizeof(text), "You picked up the %s.", item_name(item));
        game_show_message(text);
        menu_picture(item);
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
    case SCE_SAVE:
        use_typewriter();
        break;
    case SCE_ITEMBOX:
        menu_item_box(item_box, BOX_SLOTS, choice_frame);
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
    if (!blocked(player.x, player.z, player.x + dx, player.z) &&
        !enemy_blocks_player(player.x, player.z, player.x + dx, player.z))
        player.x += dx;
    if (!blocked(player.x, player.z, player.x, player.z + dz) &&
        !enemy_blocks_player(player.x, player.z, player.x, player.z + dz))
        player.z += dz;
    update_height();
}

// every DS frame: state, animation choice and turning
// ---------------------------------------------------------------- weapons
// pl_move_05_aim (docs/player-control.md 5.): R raises the weapon in
// hand (PLW move 9), the auto-aim turns Leon to the nearest enemy he sees
// (rate 0xd8, FUN_8003ed74); up / down aim high / low; A fires (moves 10 /
// 12 / 14, held 11 / 13 / 15). Damage near / mid / far from table 0x8009c470.
enum { PLW_RAISE = 9 };
static const uint8_t plw_fire[3] = { 10, 12, 14 }, plw_hold[3] = { 11, 13, 15 };
// A shot plays its whole animation (phase 2, 0x800403ec); holding A in phase
// 1 fires the next one, so the rate is the animation length (VP70: 23 keys).
// Releasing R lowers the weapon only after keyframe lower_key (table
// 0x8009eed8, 3rd byte & 0x7f).
// Reloading (phase 4, 0x8004237c) plays PLW move 16; its keyframe flags
// (EDD, u32 entry | flags << 12) start a sound of the weapon bank (0x100000,
// number in 0xf0000) and load the magazine (0x400000, FUN_80065780): read from
// PL00W02 / W04.PLW (tools: pld2json.parse_edd).
// Shots: the frame handler (0x8009f0bc[weapon], VP70 0x80040b94) spawns the
// fire at the muzzle on keyframe 1 (effect 1, 138 / 615 / 8 from the hand)
// and the casing on 3 (effect 2, 350 / 40 / -15, thrown at facing + 1300).
// Per weapon from the PS1 data: the reload events of PLW move 16 (EDD flags
// 0x100000 sound, 0x400000 loads), the fire moves' own sound events (the
// shotgun pumps on key 23 / 26 / 26, flag 0x140000: sound 4) and the
// lowering key of table 0x8009eed8 (row item - 1, 3rd byte).
typedef struct {
    uint8_t item;
    uint16_t range;           // 0 = any
    uint16_t damage[3];       // zombie table 0x8009c470: near / middle / far
    uint8_t lower_key;
    uint8_t reload_key[3], reload_sound[3]; // key 0 = no event
    uint8_t load_event;       // which reload event loads
    uint8_t pump_key[3];      // fire move keyframe (straight / up / down) with a sound; 0 = none
    uint8_t pump_sound;
} Weapon;
static const Weapon weapons[] = {
    { 0x01, 1500, { 3, 0, 0 }, 50, { 0 }, { 0 }, 1, { 0 }, 0 },                       // knife
    { 0x02, 0, { 16, 15, 14 }, 8, { 7, 22 }, { 4, 3 }, 1, { 0 }, 0 },                  // H&K VP70
    { 0x03, 0, { 16, 15, 14 }, 8, { 7, 22 }, { 4, 3 }, 1, { 0 }, 0 },                  // Browning (no move 16 in its PLW)
    { 0x04, 0, { 16, 15, 14 }, 15, { 7, 23 }, { 3, 3 }, 1, { 0 }, 0 },                 // custom handgun
    { 0x05, 0, { 900, 900, 900 }, 30, { 7, 22 }, { 4, 3 }, 1, { 0 }, 0 },              // magnum
    { 0x06, 0, { 900, 900, 900 }, 50, { 7, 22 }, { 4, 3 }, 1, { 0 }, 0 },              // custom magnum
    { 0x07, 0, { 200, 60, 40 }, 30, { 28, 42 }, { 3, 3 }, 0, { 23, 26, 26 }, 4 },      // shotgun
    { 0x08, 0, { 300, 80, 60 }, 40, { 27, 42, 56 }, { 3, 3, 3 }, 0, { 0 }, 0 },        // custom shotgun
};
enum { PLW_RELOAD = 16 };
#define SND_EMPTY 1               // ARMS sound 1: the click (FUN_80057364(0x1010001))
#define HAND_BONE 11
static int leon_weapon = -1;      // item whose model / sound bank are loaded
static void *weapon_bank;         // ARMSnn (PS1 bank 1): the shot is sound 0

static const Weapon *weapon_in_hand(void)
{
    if (inventory_equipped < 0 || !inventory[inventory_equipped].item)
        return NULL;
    for (unsigned i = 0; i < sizeof(weapons) / sizeof(weapons[0]); i++)
        if (weapons[i].item == inventory[inventory_equipped].item)
            return &weapons[i];
    return NULL;
}

// The weapon's own texture (model/leon_wNN_tex.bin, tools/plw2ds.py): a
// corner of one of Leon's pages and some palette colours, as the PS1 copies
// the PLW's TIM into VRAM.
static void apply_weapon_texture(int item)
{
    char path[40];
    uint32_t size;
    snprintf(path, sizeof(path), "nitro:/model/leon_w%02x_tex.bin", item);
    uint16_t *t = asset_load(path, &size);
    if (!t)
        return;
    int page = t[0], x = t[1], y = t[2], w = t[3], h = t[4], first = t[5], n = t[6];
    const uint16_t *colors = t + 8;
    const uint8_t *pix = (const uint8_t *)(colors + n);
    uint8_t *tex = page < leon.header->num_pages ? glGetTexturePointer(leon_tex[page]) : NULL;
    if (tex && size >= 16u + 2 * n + w * h && !(x & 1) && !(w & 1) && x + w <= TEX_PAGE_WIDTH &&
        y + h <= TEX_PAGE_HEIGHT && first + n <= 256) {
        DC_FlushRange(t, size);
        glBindTexture(0, leon_tex[page]);
        glColorSubTableEXT(0, first, n, 0, 0, colors);
        for (int r = 0; r < h; r++)
            texture_write(tex + (y + r) * TEX_PAGE_WIDTH + x, pix + r * w, w);
    }
    asset_free(t);
}

// Leon's model follows the weapon in hand (tools/plw2ds.py: PLW moves and hand)
static void update_weapon_model(void)
{
    int item = inventory_equipped >= 0 ? inventory[inventory_equipped].item : 0;
    if (item == leon_weapon)
        return;
    char path[40];
    Model m;
    snprintf(path, sizeof(path), "nitro:/model/leon_w%02x.bin", item);
    int tex_item = item;
    if (!model_load(&m, path)) {
        if (!model_load(&m, "nitro:/model/leon_w00.bin"))
            return;
        tex_item = 0;
    }
    asset_free(leon.data);
    leon = m;
    build_hierarchy();
    apply_weapon_texture(tex_item);
    leon_weapon = item;
    sound_bank_free(weapon_bank);
    snprintf(path, sizeof(path), "nitro:/sound/arms%02x.bin", item);
    weapon_bank = item ? sound_bank_load(path) : NULL;
    if (player.set == &leon && player.anim >= leon.header->num_anims)
        player.anim = ANIM_IDLE;
}

// A point in the frame of one of Leon's bones, in the world: the pose as
// drawn, M = T(pos) * Ry(dir) * ... * M_parent * T(bone) * Rx * Ry * Rz
static void leon_point(int bone, const int32_t local[3], int32_t out[3])
{
    float p[3] = { (float)local[0], (float)local[1], (float)local[2] };
    for (int b = bone; b >= 0 && b < leon.header->num_bones; b = leon.bones[b].parent) {
        const int32_t *r = pose_rot[b < POSE_MAX_BONES ? b : 0];
        float a, c, s, t;
        a = r[2] * (6.2831853f / 32768.0f), c = cosf(a), s = sinf(a); // Rz
        t = c * p[0] - s * p[1]; p[1] = s * p[0] + c * p[1]; p[0] = t;
        a = r[1] * (6.2831853f / 32768.0f), c = cosf(a), s = sinf(a); // Ry
        t = c * p[0] + s * p[2]; p[2] = -s * p[0] + c * p[2]; p[0] = t;
        a = r[0] * (6.2831853f / 32768.0f), c = cosf(a), s = sinf(a); // Rx
        t = c * p[1] - s * p[2]; p[2] = s * p[1] + c * p[2]; p[1] = t;
        const ModelBone *bn = &leon.bones[b];
        for (int k = 0; k < 3; k++)
            p[k] += leon.bones[b].parent < 0 ? (float)pose_ofs[k] : (float)bn->pos[k];
    }
    float a = player.dir * (6.2831853f / 32768.0f), c = cosf(a), s = sinf(a);
    out[0] = player.x + (int32_t)(c * p[0] + s * p[2]);
    out[1] = player.y + (int32_t)p[1];
    out[2] = player.z + (int32_t)(-s * p[0] + c * p[2]);
}

// keyframe effects of a shot (VP70 frame handler 0x80040b94): on key 1 the
// flash (CORE00 effect 1, 0x01000e00) and fire (effect 3 sub 2 with the next
// palette, 0x030a0e00) at the muzzle, on key 3 smoke (0x03080900) and the
// casing (0x02000700) at the ejection port
// The gun effects at a quarter of the room effects' size (size 16 = 1024/4096):
// the flash about 12 cm, the smoke a small puff instead of a cloud
#define GUN_FX_SIZE 16
// the PS1 size byte (0x0e for the VP70) relative to GUN_FX_SIZE; capped at
// the shotgun's largest flash (0x1d): the byte of 0x01025000 (0x50) may not
// be a size at all
#define GUN_FX(size) (((size) > 0x1D ? 0x1D : (size)) * GUN_FX_SIZE / 0x0E)

// a casing / shell: CORE00 effect 2 from frame first on, thrown out at
// facing + 1300 like FUN_8001b8fc's direction. The effect's frames: 0-7 the
// tumbling handgun casing (frame 8 loops to 0), 9-16 the shotgun shell (17
// loops to 9). Its sub effect table converts badly (all point at frame 22),
// so the frames are named here.
enum { CASING_HANDGUN = 0, CASING_SHELL = 9 };
static void throw_casing(int first, int dir_re2, const int32_t p[3])
{
    int16_t dir = player.dir + dir_re2 * 8;
    roomfx_throw(2, first, 0, p[0], p[1], p[2], (30 * cosLerp(dir)) >> 12, -45, -((30 * sinLerp(dir)) >> 12),
                 player.y, 4096 * 3 / 10);
}

// The frame handlers 0x8009f0bc[weapon] as data: on a fire move keyframe,
// FUN_8001b8fc(id << 24 | sub << 16 | size << 8, 0, hand matrix, point).
// Effect 2 is the casing: thrown at facing + dir from the casing frames.
typedef struct {
    uint8_t item, key, id, sub, size;
    int16_t x, y, z;
    int16_t dir;              // casings: direction offset (RE2 units)
} ShotFx;
static const ShotFx shot_fx[] = {
    // VP70 0x80040b94 (also the other handguns for now)
    { 0x02, 1, 3, 0x0A, 0x0E, 138, 615, 8, 0 }, { 0x02, 1, 1, 0x00, 0x0E, 138, 615, 8, 0 },
    { 0x02, 3, 3, 0x08, 0x0E, 350, 40, -15, 0 }, { 0x02, 3, 2, CASING_HANDGUN, 0, 350, 40, -15, 1300 },
    // magnum 0x80040ee8
    { 0x05, 1, 3, 0x0A, 0x14, 110, 670, -10, 0 }, { 0x05, 1, 1, 0x00, 0x1D, 110, 670, -10, 0 },
    { 0x05, 4, 3, 0x08, 0x09, 330, 50, 0, 0 }, { 0x05, 4, 2, CASING_HANDGUN, 0, 330, 50, 0, 0x694 },
    // custom magnum 0x80040ff4
    { 0x06, 1, 3, 0x0A, 0x18, 130, 900, -10, 0 }, { 0x06, 1, 1, 0x00, 0x20, 130, 900, -10, 0 },
    { 0x06, 1, 1, 0x02, 0x30, 130, 1100, -10, 0 },
    { 0x06, 4, 3, 0x08, 0x09, 330, 50, 0, 0 }, { 0x06, 4, 2, CASING_HANDGUN, 0, 330, 50, 0, 0x694 },
    // shotgun 0x80041138: the blast along the barrel, the pump
    { 0x07, 1, 1, 0x00, 0x1D, 150, 1300, 0, 0 }, { 0x07, 1, 1, 0x04, 0x10, 150, 1300, 0, 0 },
    { 0x07, 1, 3, 0x0C, 0x18, 150, 1300, 0, 0 }, { 0x07, 1, 3, 0x0E, 0x10, 150, 1800, 0, 0 },
    { 0x07, 1, 1, 0x02, 0x50, 150, 1800, 0, 0 }, { 0x07, 1, 3, 0x0E, 0x1D, 150, 2300, 0, 0 },
    { 0x07, 0x1A, 3, 0x08, 0x0D, 184, 558, -80, 0 }, { 0x07, 0x1A, 2, CASING_SHELL, 0, 184, 558, -80, 1300 },
    { 0x07, 0x1B, 3, 0x08, 0x0D, 184, 538, -80, 0 },
    // custom shotgun 0x80041310
    { 0x08, 1, 1, 0x00, 0x29, 780, 1330, 0, 0 }, { 0x08, 1, 1, 0x04, 0x16, 780, 1330, 0, 0 },
    { 0x08, 1, 1, 0x05, 0x40, 780, 1330, 0, 0 }, { 0x08, 1, 3, 0x0C, 0x16, 780, 1330, 0, 0 },
    { 0x08, 1, 3, 0x0E, 0x0E, 959, 1630, 0, 0 }, { 0x08, 1, 1, 0x04, 0x18, 959, 1630, 0, 0 },
    { 0x08, 1, 3, 0x0E, 0x16, 1253, 2130, 0, 0 }, { 0x08, 1, 1, 0x03, 0x59, 1253, 2130, 0, 0 },
    { 0x08, 1, 1, 0x04, 0x1A, 1253, 2130, 0, 0 }, { 0x08, 1, 3, 0x0E, 0x20, 1606, 2730, 0, 0 },
    { 0x08, 5, 3, 0x08, 0x0D, 184, 558, -80, 0 }, { 0x08, 5, 2, CASING_SHELL, 0, 184, 558, -80, 1300 },
    { 0x08, 6, 3, 0x08, 0x0D, 184, 538, -80, 0 },
};

static void shot_effects(const Weapon *w)
{
    int item = w->item == 0x03 || w->item == 0x04 ? 0x02 : w->item; // handguns share the VP70's
    for (unsigned i = 0; i < sizeof(shot_fx) / sizeof(shot_fx[0]); i++) {
        const ShotFx *f = &shot_fx[i];
        if (f->item != item || f->key != player.key)
            continue;
        const int32_t local[3] = { f->x, f->y, f->z };
        int32_t p[3];
        leon_point(HAND_BONE, local, p);
        if (f->id == 2)
            throw_casing(f->sub, f->dir, p);
        else
            roomfx_spawn(f->id, f->sub, p[0], p[1], p[2], GUN_FX(f->size), 0);
    }
}

// phase 4 (0x8004237c): plays move 16, sounds and loading on its keyframes;
// back to aiming at the end
static void start_reload(void)
{
    player.state = ST_RELOAD;
    player.set = &leon;
    player.anim = PLW_FIRST + PLW_RELOAD;
    player.key = 0;
}

static void reload_tick(void)
{
    const Weapon *w = weapon_in_hand();
    if (!w || PLW_FIRST + PLW_RELOAD >= leon.header->num_anims) { // no move for it: at once
        if (w)
            sound_play(weapon_bank, w->reload_sound[w->load_event], 127, 64);
        inventory_reload();
        player.state = ST_AIM;
        return;
    }
    for (int k = 0; k < 3; k++)
        if (w->reload_key[k] && player.key == w->reload_key[k]) {
            sound_play(weapon_bank, w->reload_sound[k], 127, 64);
            if (k == w->load_event)
                inventory_reload();
        }
    const ModelAnim *a = &leon.anims[player.anim];
    if (player.key + 1 < a->count) {
        player.key++;
    } else {
        player.state = ST_AIM;
        player.aim_raised = 1;
        player.anim = PLW_FIRST + plw_hold[player.aim_posture];
        player.key = 0;
    }
}

static void fire(void)
{
    const Weapon *w = weapon_in_hand();
    InvSlot *slot = &inventory[inventory_equipped];
    if (!w)
        return;
    if (w->item != 0x01) {
        if (slot->count == 0)
            return;
        slot->count--;
    }
    player.state = ST_FIRE;
    player.set = &leon;
    player.anim = PLW_FIRST + plw_fire[player.aim_posture];
    player.key = 0;
    player.shot = 1;
    sound_play(weapon_bank, 0, 127, 64);
    enemy_hit(player.x, player.y, player.z, player.dir, w->range, w->item, w->damage, player.aim_posture == 2);
}

// aiming / firing per DS frame; returns 1 while the weapon is up
static int update_aim(uint32_t held, uint32_t down)
{
    const Weapon *w = weapon_in_hand();
    if (player.state == ST_RELOAD)
        return 1; // the reload plays to its end (phase 4 checks no buttons)
    if (!w || (!(held & KEY_R) && !(player.state == ST_FIRE && player.key <= w->lower_key))) {
        if (player.state == ST_AIM || player.state == ST_FIRE)
            player.state = ST_IDLE;
        return 0;
    }
    if (player.state != ST_AIM && player.state != ST_FIRE) {
        player.state = ST_AIM;
        player.aim_raised = 0;
        player.set = &leon;
        player.anim = PLW_FIRST + PLW_RAISE;
        player.key = 0;
        player.aim_target = enemy_aim_target(player.x, player.y, player.z, w->range ? w->range * 3 : 0,
                                             &player.aim_x, &player.aim_z);
    }
    player.aim_posture = (held & KEY_UP) ? 1 : (held & KEY_DOWN) ? 2 : 0;
    if (player.aim_target >= 0 && !player.aim_raised) {
        int want = (int)(atan2f((float)-(player.aim_z - player.z), (float)(player.aim_x - player.x)) *
                         32768.0f / (2.0f * 3.14159265f)) & 32767;
        int diff = ((want - (player.dir & 32767) + 16384) & 32767) - 16384, step = TURN_PER_FRAME(0xD8);
        player.dir += diff > step ? step : diff < -step ? -step : diff;
    } else {
        int turn = TURN_PER_FRAME(player.state == ST_FIRE ? 8 : 64);
        if (held & KEY_RIGHT)
            player.dir += turn;
        if (held & KEY_LEFT)
            player.dir -= turn;
    }
    if (player.state == ST_AIM && player.aim_raised)
        player.anim = PLW_FIRST + plw_hold[player.aim_posture], player.key = 0;
    // phase 1 (0x80040128): a loaded weapon fires; an empty one reloads on a
    // new press if there is ammunition, else it clicks
    if ((held & KEY_A) && (held & KEY_R) && player.state == ST_AIM && player.aim_raised) {
        if (w->item == 0x01 || inventory[inventory_equipped].count > 0)
            fire();
        else if (!(down & KEY_A))
            ;
        else if (inventory_spare_ammo())
            start_reload();
        else
            sound_play(weapon_bank, SND_EMPTY, 127, 64);
    }
    return 1;
}

static void update_player_frame(uint32_t held, uint32_t down)
{
    if (player.state >= ST_CLIMB)
        return; // special move or the room script in progress: no control
    if (flag_get(2, 7))
        held = down = 0; // a cutscene runs (the scripts set flag 2:7 around them)
    update_weapon_model();
    if (update_aim(held, down))
        return;
    PlayerState st = ST_IDLE;
    if (held & KEY_UP)
        st = (held & KEY_B) ? ST_RUN : ST_WALK;
    else if (held & KEY_DOWN)
        st = ST_BACK;
    else if (held & (KEY_LEFT | KEY_RIGHT))
        st = ST_TURN; // pl_move_04: walks on the spot while turning
    player.state = st;

    int h = health_level();
    int turn = TURN_PER_FRAME(st == ST_RUN ? turn_run[h] : st == ST_BACK ? turn_back[h]
                              : st == ST_WALK ? turn_walk[h] : turn_spot[h]);
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
    if (!rbj_moves)
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
    if (st >= ST_GRABBED)
        return; // enemy.c moves him
    if (st == ST_SCRIPT) {
        script_move_tick();
        return;
    }
    if (st >= ST_CLIMB) {
        special_move_tick();
        return;
    }
    if (st == ST_RELOAD) {
        reload_tick();
        return;
    }
    if (st == ST_AIM || st == ST_FIRE) {
        const ModelAnim *wa = &leon.anims[player.anim];
        if (player.key + 1 < wa->count) {
            player.key++;
            const Weapon *w = st == ST_FIRE ? weapon_in_hand() : NULL;
            if (w && w->item != 0x01) {
                shot_effects(w);
                // EDD flag 0x100000 on the fire move: the pump
                if (w->pump_key[player.aim_posture] && player.key == w->pump_key[player.aim_posture])
                    sound_play(weapon_bank, w->pump_sound, 127, 64);
            }
        }
        else if (st == ST_AIM)
            player.aim_raised = 1;
        else
            player.state = ST_AIM; // shot over: back to holding
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

    if (st != ST_IDLE && st != ST_TURN) {
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
    case SCE_DOOR: return "Door";
    case SCE_ITEM: return "Item";
    case SCE_MESSAGE: return "Text";
    case SCE_EVENT: return "Event";
    default: return "?";
    }
}

static void print_status(void)
{
    if (menu_sel >= 0 && menu_open)
        return; // the room list uses the console
    printf("\x1b[0;0HRE2 DS - room %X%02X  cam %d   \n", cur_stage, cur_room, cur_cam);
    printf("pos %6ld %6ld %6ld  \n", (long)player.x, (long)player.y, (long)player.z);
    printf("dir %5d floor %d slope %3d  \n", player.dir & 0x7FFF, player_floor(), player.slope);
    printf("skript: %d threads, %d unbek.  \n", script_threads_running, script_unknown_ops);
    printf("HP %3d %s  gegner %d  \n", player.hp,
           player.hp < 21 ? "Danger " : player.hp < 101 ? "Caution" : "Fine   ", enemy_count());
    printf("cull %s gx %4d/%4d   \n",
           cull_models == POLY_CULL_BACK ? "back " : cull_models == POLY_CULL_FRONT ? "front" : "aus  ",
           gx_vertices_last, gx_polygons_last);
    printf("fx %d st %d n %d    \n", effect_count(), effect_status, effect_spawned);
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
    printf("\nD-pad walk/turn, B run\nA use, L+A open any lock\nL+A at a typewriter: save\nSELECT room menu\n");
}

static void update_menu(uint32_t down)
{
    if (down & KEY_SELECT) {
        menu_open = !menu_open;
        menu_debug(menu_open);
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
        menu_debug(0);
        if (down & KEY_A)
            warp_to(menu_sel);
        return;
    }

    printf("\x1b[0;0HChoose a room (%d rooms)\n", room_count);
    printf("up/down, L/R +-10\nA warp, B back\n\n");
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
    // Default slots only: libnds derives the texture slot from the bank
    // number (A = slot 0, D = slot 3) when it uploads. 512 KB of textures:
    // Leon, the masks, the effects and up to four enemy types at once.
    vramSetBankA(VRAM_A_TEXTURE);
    vramSetBankB(VRAM_B_TEXTURE);
    vramSetBankC(VRAM_C_TEXTURE);
    vramSetBankD(VRAM_D_TEXTURE);
    vramSetBankE(VRAM_E_MAIN_BG);          // the background bitmap
    vramSetBankG(VRAM_G_TEX_PALETTE_SLOT0); // 16 KB: 32 palettes of 256 colours
    // sub screen text on VRAM H (32 KB): font tiles at 0, map at 8 KB
    videoSetModeSub(MODE_0_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 4, 0, false, true);

    if (!nitro_init()) {
        consoleDemoInit();
        iprintf("\x1b[1;1HERROR:\nNitroFS init failed (step %d,\nboot %d)\n\n"
                "With DLDI (melonDS) copy\nre2ds.nds to the SD card root\n(step 4: the copy there is\nanother build).\n",
                nitro_step, g_envBootParam->boot_src);
        while (1)
            swiWaitForVBlank();
    }
    save_init(); // re2ds.sav on the SD card; without one the game just cannot save
    sound_init();

    bg_id = bgInit(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
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
    reserve_mask_texture();
    calibrate_culling();
    load_room_index();

    effect_load();
    menu_init();
    voice_bank = sound_bank_load("nitro:/sound/core00.bin");
    enemy_pairs = asset_load("nitro:/sound/enem_pairs.bin", NULL);
    // as the PS1 boots: the title movie (PL0/ZMOVIE/TITLEL.STR: the CAPCOM
    // logo, game scenes, the G eye, the announcer's "Resident Evil 2"), then
    // the title screen; the opening comes with NEW GAME (start_game). START
    // skips a movie; holding SELECT while the game starts leaves the movies
    // out (debugging).
    scanKeys();
    skip_movies = (keysHeld() & KEY_SELECT) != 0;
    if (!skip_movies)
        movie_play("titlel");
    movie_end();
    setBrightness(1, -16);
    start_game();

    // pmMainLoop() services calico's power management (lid, reset, ARM7 messages);
    // the devkitPro examples call it every frame before reading input.
    while (pmMainLoop()) {
        scanKeys();
        uint32_t down = keysDown();
        // the status screen pauses play while it has the focus or shows a
        // message; the closing press is not passed on (it would trigger the
        // same AOT again)
        int paused = 0;
        if (!menu_open && menu_has_focus() && !menu_message_open() && (down & KEY_L)) {
            open_files();
            down = 0;
        }
        if (!menu_open) {
            int had_message = menu_message_open();
            paused = menu_frame(down, player.hp) || had_message;
            if (!menu_message_open())
                message_text = NULL;
            if (paused)
                down = 0;
        }
        if (!paused)
            update_menu(down);
        if (!menu_open && !paused) {
            pending_keys |= down;
            update_player_frame(keysHeld(), down);
            for (game_tick_acc += GAME_HZ; game_tick_acc >= DS_HZ && !message_text; game_tick_acc -= DS_HZ) {
                update_player_tick();
                EnemyTarget target = {
                    .x = player.x, .y = player.y, .z = player.z, .dir = player.dir,
                    .noise = (player.shot ? NOISE_SHOT : 0) | (player.state == ST_RUN ? NOISE_RUN
                             : player.state == ST_WALK || player.state == ST_BACK || player.state == ST_TURN
                               ? NOISE_WALK : 0),
                    .pressed = (pending_keys & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A |
                                                KEY_B | KEY_X | KEY_Y | KEY_L | KEY_R)) != 0,
                    .can_grab = player.state < ST_CLIMB,
                    .dead = player.state == ST_DEAD,
                };
                enemy_tick(&target);
                update_pose();
                die_tick();
                effect_tick();
                roomfx_tick();
                player.shot = 0;
                if (player.state < ST_SCRIPT && player.state != ST_AIM && player.state != ST_FIRE && player.state != ST_RELOAD &&
                    !flag_get(2, 7))
                    check_aots(pending_keys);
                if (!near_aot && !message_text)
                    try_special_move(pending_keys);
                pending_keys = 0;
                script_tick();
            }
        }
        if (menu_open)
            print_status();
        render_frame();
        swiWaitForVBlank();
        music_frame();
        voice_frame();
        music_duck(voice_playing());
    }
    return 0;
}
