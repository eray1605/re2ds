// re2ds - data formats produced by tools/*2ds.py for the DS engine.
//
// All files are little endian. World coordinates are RE2 game units, used
// directly as 20.12 fixed point on the DS (4096 game units = 1.0). The RE2
// world has -Y pointing up; the engine keeps that convention.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Backgrounds: bg/<room>_camNN.bin (<room> = "room" + stage + room in hex,
// e.g. room10a)
//   BgHeader, then the 256 x 192 8 bpp image (row-major, rows of 256 bytes)
//   LZ77-compressed in the DS BIOS format (decompress(..., LZ77Vram)).
//   Index 0 is unused: it is transparent on an 8 bpp bitmap background.
#define BG_WIDTH 256
#define BG_HEIGHT 192
#define BG_MAGIC 0x31474252 // "RBG1"

typedef struct {
    uint32_t magic;
    uint16_t palette[256];  // BGR555
    // LZ77 stream follows
} BgHeader;

// ---------------------------------------------------------------------------
// Foreground masks: mask/<room>_camNN.bin (only for cameras that have masks)
//   The file is LZ77-compressed (DS BIOS format); unpacked it holds the
//   layout below.
//   Each mask rectangle is a textured quad placed in the world at its PS1
//   depth (distance along the view direction = depth * 32). Drawn with the
//   room camera after the models, the hardware depth test hides whatever is
//   behind the foreground object.
#define MASK_MAGIC 0x314B4D52 // "RMK1"

typedef struct {
    uint32_t magic;
    uint16_t num_quads;
    uint16_t tex_height;    // texture height, power of two (width is 256)
    uint16_t used_height;   // rows actually used by the atlas
    uint16_t reserved;
    // uint16_t palette[256];               BGR555, index 0 = transparent
    // uint8_t  pixels[256 * tex_height];   8 bpp indices, row-major
    // MaskQuad quads[num_quads];
} MaskHeader;

typedef struct {
    uint8_t u, v;           // top-left texel in the atlas
    uint16_t w, h;          // size in texels (= PS1 screen pixels)
    uint16_t depth;         // PS1 depth value (distance / 32), for reference
    int32_t corner[4][3];   // world positions: top-left, top-right,
                            // bottom-right, bottom-left (as seen on screen)
} MaskQuad;                 // 56 bytes

// ---------------------------------------------------------------------------
// Room: room/<rdt>.bin (<rdt> = "room" + stage + room in hex + player, e.g.
// room10a0 for Leon). room/index.bin lists the converted rooms: u16 count,
// then count x (u8 stage, u8 room).
#define ROOM_MAGIC 0x314D5252 // "RRM1"
#define ROOM_VERSION 4

typedef struct {
    uint32_t magic;
    uint16_t version;      // ROOM_VERSION
    uint8_t num_cameras;
    uint8_t num_switches;
    uint16_t num_boxes;
    uint8_t start_camera;  // camera whose view area contains the spawn point
    uint8_t num_doors;
    int32_t spawn[3];      // debug spawn point x, y, z (y = floor height)
    int16_t spawn_dir;     // facing, DS angle units (32768 = 360 degrees).
                           // Models face +X; the model matrix is T(pos) * Ry(dir)
                           // with Ry = [[c,0,s],[0,1,0],[-s,0,c]] (PS1 RotMatrixY),
                           // so the facing vector is (cos dir, 0, -sin dir).
    uint16_t reserved2;
    // Room scripts (raw SCD blocks from the RDT: u16 function offsets, then
    // the bytecode) and texts. Offsets are from the start of the file.
    uint32_t off_init, size_init;   // init script, runs once on entry
    uint32_t off_exec, size_exec;   // main script, function 0 starts on entry
    uint32_t off_msgs;              // u16 offsets[num_msgs] (from off_msgs), then
    uint16_t num_msgs;              // NUL-terminated ASCII, "\n" = line break
    uint16_t reserved3;
    uint32_t off_flr;               // RoomFloorSound[num_flr]
    uint16_t num_flr;
    uint16_t reserved4;
    // followed by RoomCamera[num_cameras], RoomSwitch[num_switches],
    // RoomBox[num_boxes], RoomDoor[num_doors] (doors as found by the
    // converter; the engine takes doors from the scripts instead)
} RoomHeader;                       // 60 bytes

// Floor sound area (RDT FLR). Footsteps play entry sound + 0 (walk) or + 1
// (run) of the room's sound bank, sound/<rdt>.bin (FUN_8004f078 / 8005797c).
#define FLOOR_SOUND_DEFAULT 29
typedef struct {
    int16_t x, z;
    uint16_t w, d;
    uint16_t sound;
    uint16_t floor;                 // & 0x7F = floor level, & 0x80 = disabled
} RoomFloorSound;                   // 12 bytes

typedef struct {
    int32_t from[3];       // eye
    int32_t to[3];         // look-at point
    uint16_t screen;       // RID distance >> 7; projection on a 320x240 screen
    uint16_t flag;
} RoomCamera;              // 28 bytes

// Camera switch zone on the floor (x/z quad). See world/cameraswitch.go in
// OpenBiohazard2: per camera, the first zone with cam1 == 0 is that camera's
// own view area and never switches.
#define SWITCH_VIEW_AREA 0x01  // role bit: this zone is cam0's view area
typedef struct {
    uint8_t cam0;          // active camera
    uint8_t cam1;          // camera to switch to
    uint8_t floor;         // 255 = any floor
    uint8_t role;          // SWITCH_VIEW_AREA or 0 (= transition)
    int16_t x[4];
    int16_t z[4];
} RoomSwitch;              // 20 bytes

typedef struct {
    int16_t x, z;          // corner
    uint16_t w, d;         // size along x / z
    uint16_t shape;        // 0 = box, 1..10 = shapes (triangles, circle, ...),
                           // 11 = slope, 12 = stairs (see OpenBiohazard2 world/collision.go)
    uint16_t flag;         // bits 0..3 = shape; the upper bits say who collides
                           // (BOX_*; FUN_80048964 tests flag & mask); 0 = disabled
                           // (scripts toggle it with sca_id_set)
    uint16_t type;         // floor height multiplier in bits 6..10 etc.
    uint16_t reserved;
    uint32_t floors;       // bit n = present on floor level n
} RoomBox;                 // 20 bytes

// RoomBox.flag: whom the box stops. The movement collision (0x80033224 ->
// FUN_80048964) is called with mask 0x8000 for the player (0x80025b60) and
// 0x400 by the zombie code (0x80100650); other objects use 0x800 / 0x1000.
// Room 103's stair railings (0x5E00) stop zombies but not Leon.
#define BOX_PLAYER 0x8000
#define BOX_ENEMY 0x0400

// Floor levels: y = level * FLOOR_HEIGHT (-Y is up).
#define FLOOR_HEIGHT (-1800)
// RoomBox.shape values that are walkable and change the height
#define SHAPE_SLOPE 11
#define SHAPE_STAIRS 12
// RoomBox.shape values for the special moves (press A in front of them)
#define SHAPE_CLIMB_UP 9      // e.g. bins and crates: climb one level up
#define SHAPE_JUMP_DOWN 10    // edge on the upper level: jump one level down

// Door AOT from the room scripts (DOOR_AOT_SET / DOOR_AOT_SET_4P). Locks and
// conditions are not evaluated yet (needs the script interpreter).
#define AOT_SAT_PLAYER 0x01   // triggered by the player
#define AOT_SAT_MANUAL 0x10   // needs the action button (else: walk into it)
#define AOT_SAT_FRONT  0x20   // player must face it
typedef struct {
    int16_t x[4], z[4];    // trigger quad
    int16_t next[3];       // position in the target room
    uint16_t next_dir;     // facing in the target room, DS units
    uint8_t next_stage;    // 1..7
    uint8_t next_room;
    uint8_t next_camera;
    uint8_t next_floor;
    uint8_t sat;           // AOT_SAT_* bits
    uint8_t floor;         // floor level of the trigger
    uint8_t door_type;     // door animation (DO2), not used yet
    uint8_t key_id;        // 0 = not locked
    uint8_t key_type;
    uint8_t aot;           // AOT slot in the room
    uint8_t reserved[2];
} RoomDoor;                // 36 bytes

// ---------------------------------------------------------------------------
// Sound bank: sound/doorNN.bin (tools/vab2ds.py), one per door file DOORNN.DO2
//   SoundBankHeader, SoundEntry[count], then 16-bit PCM. Each entry covers a
//   key range of the PS1 VAB; the door program keys on note 60 (open) and
//   61 (close).
#define SOUND_MAGIC 0x31534252 // "RBS1"

typedef struct {
    uint32_t magic;
    uint16_t count;
    uint16_t reserved;
} SoundBankHeader;

typedef struct {
    uint8_t note_lo, note_hi;
    uint16_t rate;          // Hz
    uint32_t offset;        // from the start of the file, 4-byte aligned
    uint32_t size;          // bytes of 16-bit PCM
} SoundEntry;               // 12 bytes

// ---------------------------------------------------------------------------
// Model: model/<name>.bin
#define MODEL_MAGIC 0x31444D52 // "RMD1"

typedef struct {
    uint32_t magic;
    uint16_t num_bones;
    uint16_t num_pages;     // texture pages (one 128x256 8bpp texture each)
    uint16_t num_frames;    // keyframes
    uint16_t num_anims;
    uint32_t off_bones;     // ModelBone[num_bones]
    uint32_t off_lists;     // u32 offsets[num_bones * num_pages] to display lists (0 = none)
    uint32_t off_frames;    // ModelFrame[num_frames], each followed by
                            // int16 rot[num_bones][3]
    uint32_t off_anims;     // ModelAnim[num_anims]
} ModelHeader;

typedef struct {
    int8_t parent;          // -1 for the root
    uint8_t reserved;
    int16_t pos[3];         // offset from the parent (bind pose)
} ModelBone;                // 8 bytes

typedef struct {
    int16_t offset[3];      // root position for this keyframe (replaces bone 0 pos)
    int16_t speed[3];       // root motion: displacement since the animation's
                            // first keyframe, model space (walk: +X ~80/frame)
    // int16_t rot[num_bones][3];  // X, Y, Z in DS angle units (32768 = 360 deg)
    //                             // pose: M = M_parent * T(pos) * Rx * Ry * Rz
} ModelFrame;

typedef struct {
    uint16_t first;         // index into the anim frame-index pool
    uint16_t count;         // keyframes in this animation
} ModelAnim;                // pool of u16 entries follows the anim table:
                            // frame index | flags << 12
#define ANIM_FRAME(e) ((e) & 0x0FFF)
#define ANIM_FLAG_STEP 0x4000      // footstep on this keyframe
#define ANIM_FLAG_STEP_ALT 0x2000  // ... with the other foot

// Display lists (one per bone and texture page) are packed GX FIFO command
// lists for glCallList(): u32 word count, then the words. They contain
// BEGIN/TEXCOORD/VTX_16/END only. Vertices are bone-local game units; texture
// coordinates are texels within the 128x256 page (t16 = texel * 16).

// ---------------------------------------------------------------------------
// Texture: model/<name>_tex.bin
//   num_pages times: u16 palette[256] (BGR555), then 128*256 u8 indices
//   (row-major). Colour 0 of each palette is opaque (PS1 STP semantics are
//   ignored for now).
#define TEX_PAGE_WIDTH 128
#define TEX_PAGE_HEIGHT 256
