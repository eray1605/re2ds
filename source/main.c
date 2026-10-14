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
#define FADE_STEPS 8        // frames per fade (brightness 0 -> -16)

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
static const RoomDoor *near_door;   // door the action button would open

static struct {
    int32_t x, y, z;
    int16_t dir;
    PlayerState state;
    int anim, key;
    int tick;                 // accumulates GAME_HZ per DS frame, a game tick every DS_HZ
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
    near_door = NULL;
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
        if (!(b->floors & floor_bit) || is_slope(b))
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

static void check_camera_switch(void)
{
    int floor = player_floor();
    for (int i = 0; i < room.header->num_switches; i++) {
        const RoomSwitch *s = &room.switches[i];
        if (s->cam0 != cur_cam || (s->role & SWITCH_VIEW_AREA) || s->cam1 == cur_cam)
            continue;
        if (s->floor != 255 && s->floor != floor)
            continue;
        if (point_in_quad(player.x, player.z, s->x, s->z)) {
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

static void place_player(int32_t x, int32_t y, int32_t z, int16_t dir, int cam)
{
    player.x = x;
    player.y = y;
    player.z = z;
    player.dir = dir;
    player.slope = -1;
    player.anim = -1;
    set_anim(ANIM_IDLE);
    set_camera(cam < room.header->num_cameras ? cam : 0);
}

// Locks are ignored for now: every door opens (needs the script interpreter).
static void enter_door(const RoomDoor *d)
{
    RoomDoor door = *d; // the room data is freed by load_room
    fade(1);
    if (load_room(door.next_stage, door.next_room))
        place_player(door.next[0], door.next[1], door.next[2], (int16_t)door.next_dir, door.next_camera);
    fade(-1);
}

static void warp_to(int index)
{
    const RoomRef *r = &room_index[index];
    fade(1);
    if (load_room(r->stage, r->room)) {
        const RoomHeader *h = room.header;
        place_player(h->spawn[0], h->spawn[1], h->spawn[2], h->spawn_dir, h->start_camera);
    }
    fade(-1);
}

// Doors the player walks into open at once; doors with AOT_SAT_MANUAL need
// the action button (A) while Leon stands in or faces the trigger area.
static void check_doors(uint32_t pressed)
{
    int floor = player_floor();
    int32_t px = player.x + ((DOOR_REACH * cosLerp(player.dir)) >> 12);
    int32_t pz = player.z - ((DOOR_REACH * sinLerp(player.dir)) >> 12);
    near_door = NULL;
    for (int i = 0; i < room.header->num_doors; i++) {
        const RoomDoor *d = &room.doors[i];
        if (!(d->sat & AOT_SAT_PLAYER) || (d->floor != 255 && d->floor != floor))
            continue;
        int inside = point_in_quad(player.x, player.z, d->x, d->z);
        if (!(d->sat & AOT_SAT_MANUAL)) {
            if (inside) {
                enter_door(d);
                return;
            }
            continue;
        }
        if (inside || point_in_quad(px, pz, d->x, d->z)) {
            near_door = d;
            if (pressed & KEY_A) {
                enter_door(d);
                return;
            }
        }
    }
}

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

static void update_player(uint32_t held)
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

    player.tick += GAME_HZ;
    if (player.tick < DS_HZ)
        return;
    player.tick -= DS_HZ;

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
static void print_status(void)
{
    printf("\x1b[0;0HRE2 DS - room %X%02X  cam %d   \n", cur_stage, cur_room, cur_cam);
    printf("pos %6ld %6ld %6ld  \n", (long)player.x, (long)player.y, (long)player.z);
    printf("dir %5d floor %d slope %3d  \n", player.dir & 0x7FFF, player_floor(), player.slope);
    if (near_door)
        printf("A: Tuer -> %X%02X            \n", near_door->next_stage, near_door->next_room);
    else
        printf("                          \n");
    printf("\nD-Pad gehen/drehen, B rennen\nA Tuer, SELECT Raum-Menue\n");
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
    if (!load_room(START_STAGE, START_ROOM))
        fatal("room/room1000.bin missing");
    const RoomHeader *h = room.header;
    place_player(h->spawn[0], h->spawn[1], h->spawn[2], h->spawn_dir, h->start_camera);
    fade(-1);

    // pmMainLoop() services calico's power management (lid, reset, ARM7 messages);
    // the devkitPro examples call it every frame before reading input.
    while (pmMainLoop()) {
        scanKeys();
        uint32_t down = keysDown();
        update_menu(down);
        if (!menu_open) {
            update_player(keysHeld());
            check_doors(down);
            print_status();
        }
        render_frame();
        swiWaitForVBlank();
    }
    return 0;
}
