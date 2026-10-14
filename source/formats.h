// re2ds - data formats produced by tools/*2ds.py for the DS engine.
//
// All files are little endian. World coordinates are RE2 game units, used
// directly as 20.12 fixed point on the DS (4096 game units = 1.0). The RE2
// world has -Y pointing up; the engine keeps that convention.
#pragma once

#include <stdint.h>

// ---------------------------------------------------------------------------
// Backgrounds: bg/<room>_camNN.bin
//   256 x 192 pixels, u16 each, BGR555 with bit 15 set (direct-colour bitmap
//   for a 16-bit bitmap background). 98304 bytes, no header.
#define BG_WIDTH 256
#define BG_HEIGHT 192

// ---------------------------------------------------------------------------
// Foreground masks: mask/<room>_camNN.bin (only for cameras that have masks)
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
// Room: room/<room>.bin
#define ROOM_MAGIC 0x314D5252 // "RRM1"

typedef struct {
    uint32_t magic;
    uint16_t version;      // 1
    uint8_t num_cameras;
    uint8_t num_switches;
    uint16_t num_boxes;
    uint8_t start_camera;  // camera whose view area contains the spawn point
    uint8_t reserved;
    int32_t spawn[3];      // x, y, z (y = floor height)
    int16_t spawn_dir;     // facing, DS angle units (32768 = 360 degrees).
                           // Models face +X; the model matrix is T(pos) * Ry(dir)
                           // with Ry = [[c,0,s],[0,1,0],[-s,0,c]] (PS1 RotMatrixY),
                           // so the facing vector is (cos dir, 0, -sin dir).
    uint16_t reserved2;
    // followed by RoomCamera[num_cameras], RoomSwitch[num_switches],
    // RoomBox[num_boxes]
} RoomHeader;

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
    uint16_t flag;
    uint16_t type;         // floor height multiplier in bits 6..10 etc.
    uint16_t reserved;
    uint32_t floors;       // bit n = present on floor level n
} RoomBox;                 // 20 bytes

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
} ModelAnim;                // pool of u16 frame indices follows the anim table

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
