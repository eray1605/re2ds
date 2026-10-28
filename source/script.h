// re2ds - interpreter for the RE2 room scripts (SCD bytecode).
//
// Each room has an init script (runs to completion on entry) and a main
// script whose function 0 starts as thread 0; more threads are started with
// EVT_EXEC or by event AOTs. Threads run once per game tick until they yield
// (sleep, evt_next, end of a loop iteration, message) or end.
//
// Block semantics (checked against all rooms with tools/scd_disasm.py --check):
//   if:    length from the end of the instruction; points past the else
//          instruction (into the else body) or past the endif.
//   else:  length from the start of the instruction to the end of the else body.
//   while: conditions follow the instruction, length points past ewhile.
//   do:    body, edwhile, then the conditions; length points past them.
//   for, switch, case: length from the end of the instruction.
//   Instructions of odd length are padded with a 0 byte (= nop).
#pragma once

#include <stdint.h>

#include "assets.h"

#define NUM_AOTS 32
#define NUM_FLAG_ARRAYS 64     // flag arrays (ck / set), 256 bits each
#define NUM_VARS 64            // script variables (cmp / save / calc)

// AOT (area of trigger) types = the "id" byte of aot_set and friends
enum {
    SCE_AUTO = 0,
    SCE_DOOR = 1,
    SCE_ITEM = 2,
    SCE_NORMAL = 3,
    SCE_MESSAGE = 4,
    SCE_EVENT = 5,
    SCE_SAVE = 9,              // typewriter
    SCE_ITEMBOX = 10,
    SCE_TERMINAL = 14,         // a computer: data[0] | data[2] << 4 picks the program (0x80036a90)
};

// Script variables the engine keeps up to date
#define VAR_CUT 0x1A           // current camera (room 102 waits for it to open the gate)

// Flag arrays with a known meaning
#define FLAGS_ITEM_PICKED 8    // bit n set = item AOT with picked index n taken

typedef struct {
    uint8_t active;
    uint8_t type;              // SCE_*
    uint8_t sat;               // AOT_SAT_* (formats.h)
    uint8_t floor;
    int16_t x[4], z[4];        // trigger quad
    uint8_t data[18];          // type specific, see below
    uint8_t inside;            // player was inside last tick (auto triggers fire on entry)
} Aot;
// data layout:
//   door:    int16 next x, y, z, dir (RE2 units); u8 stage (0-based), room,
//            camera, floor, texture, door type, knock, key id, key type
//   item:    u16 item id, amount, picked flag index; u8 model, act
//   message: u8 message id, ...
//   event:   u8 thread, ?, opcode (0x18 = gosub), function, ...

extern Aot aots[NUM_AOTS];

// Instruction sizes in bytes (opcode included), 0 = unknown; shared with the
// door program player (door.c)
extern const uint8_t scd_op_size[256];

// Engine hooks, implemented in main.c
void game_show_message(const char *text);   // pauses play until closed
void game_aot_on(int n);                     // aot_on: runs AOT n as if Leon had used it
int game_busy(void);                         // a terminal runs: scripts and play wait
int game_message_open(void);
void game_cut_change(int cam);             // force a camera
void game_set_player_pos(int32_t x, int32_t y, int32_t z);
void game_set_player_dir(int16_t dir_re2);
void game_set_box_flag(int box, int flag);  // sca_id_set: collision box on/off
// se_on: bank 0 = core/door, 2 = room (sound/<rdt>.bin), 3 = ?, 4 = weapon
// (FUN_80057364: tables 0x800d36c0[bank])
void game_play_sound(int bank, int sound);
void game_bgm_control(int id, int op, int type, int left, int right);
void game_bgmtbl_set(int stage, int room, int main, int sub); // a room's music from now on
void game_item_get(int item, int count);  // sce_item_get: into the inventory, else the item box
void game_movie(int n);                     // movie_on: plays movie table entry n
// sce_em_set: enemy / character in slot `slot` (enemy.c)
// voice: kind of voice (byte 7), picks the enemy sound set of the room
void game_spawn_enemy(int slot, int type, int flags, int voice, int x, int y, int z, int dir_re2);
void game_enemy_motion(int slot, int motion, int ctr); // sce_em_set's motion, control word
void game_enemy_skin(int slot, int model);            // sce_em_set's model byte (obj+0x1cf)
// enemy object members for the room scripts (member_set / member_copy /
// member_cmp after work_set 3): 0 if the member is not modelled
int game_enemy_member(int slot, int member, int *value);
void game_enemy_member_set(int slot, int member, int value);
// the same for Leon (work_set 1): 0x0B-0x0D position, 0x0F facing
int game_player_member(int member, int *value);
void game_player_member_set(int member, int value);
// plc_* commands (plc.h) for the work object: type 1 = Leon, 3 = enemy index;
// 0 if there is no such object to take it
int game_plc(int work_type, int index, const uint8_t *ins);
// xa_on: voice clip n of the stage (kind 0) or of the common table (kind 1)
void game_voice(int kind, int n);
int game_voice_playing(void);  // wsleeping waits for it (PS1 0x800c77d4 & 0x20)
// obj_model_set: place model ins[1] of the room (the whole 38-byte instruction)
void game_object_set(const uint8_t *ins);
// an item AOT whose item is already taken: its model stays hidden
void game_object_hide(int model);
// work_set 4: members 0x0B-0x0D position, 0x0F facing of room object n
int game_object_member(int n, int member, int *value);
// sce_espr_on (the 16-byte instruction) / sce_espr_kill: room effect sprites
void game_espr_on(const uint8_t *ins);
void game_espr_kill(int id, int type);
// flr_set: the room's floor sound area n on / off (RoomFloorSound.floor & 0x80)
void game_floor_sound_set(int n, int on);
void game_object_member_set(int n, int member, int value);

void script_room_enter(const Room *room);   // resets AOTs/threads, runs init, starts main
void script_tick(void);
void script_start_event(int function);      // run a main-script function as a new thread

int flag_get(int array, int bit);
void flag_set(int array, int bit, int value);

int cut_replace(int cam);                   // camera after cut_replace remapping
void script_camera_changed(int previous, int cam); // keeps cut_old and VAR_CUT current
int cut_auto_enabled(void);

// Leon's inventory: 8 slots, item 0 = empty; count = quantity, or the
// rounds in a weapon (0 = empty gun, still there)
#define INV_SLOTS 8
typedef struct {
    uint8_t item, count;
} InvSlot;
extern InvSlot inventory[INV_SLOTS];
extern int inventory_equipped;      // slot of the weapon in hand, -1 = none
int inventory_has(int item);
int inventory_add(int item, int amount); // 0 if there is no room
void inventory_remove(int item);
void inventory_reset(void);

// What a save game keeps of the scripts: every flag array and variable.
#define SCRIPT_STATE_SIZE (NUM_FLAG_ARRAYS * 32 + NUM_VARS * 2)
void script_state_get(uint8_t *out);
void script_state_set(const uint8_t *in);
void script_state_reset(void);      // a new game

// debug counters
extern int script_threads_running;
extern int script_stage;            // 0-based stage of the room (main.c sets it before script_room_enter)
extern int script_unknown_ops;
