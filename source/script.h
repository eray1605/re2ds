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
int game_message_open(void);
void game_cut_change(int cam);             // force a camera
void game_set_player_pos(int32_t x, int32_t y, int32_t z);
void game_set_player_dir(int16_t dir_re2);
void game_set_box_flag(int box, int flag);  // sca_id_set: collision box on/off
// se_on: bank 0 = core/door, 2 = room (sound/<rdt>.bin), 3 = ?, 4 = weapon
// (FUN_80057364: tables 0x800d36c0[bank])
void game_play_sound(int bank, int sound);
// sce_em_set: enemy / character in slot `slot` (enemy.c)
void game_spawn_enemy(int slot, int type, int flags, int x, int y, int z, int dir_re2);
// enemy object members for the room scripts (member_set / member_copy /
// member_cmp after work_set 3): 0 if the member is not modelled
int game_enemy_member(int slot, int member, int *value);
void game_enemy_member_set(int slot, int member, int value);

void script_room_enter(const Room *room);   // resets AOTs/threads, runs init, starts main
void script_tick(void);
void script_start_event(int function);      // run a main-script function as a new thread

int flag_get(int array, int bit);
void flag_set(int array, int bit, int value);

int cut_replace(int cam);                   // camera after cut_replace remapping
void script_camera_changed(int previous, int cam); // keeps cut_old and VAR_CUT current
int cut_auto_enabled(void);

// minimal inventory until the real one exists (phase 5)
int inventory_has(int item);
void inventory_add(int item, int amount);

// debug counters
extern int script_threads_running;
extern int script_unknown_ops;
