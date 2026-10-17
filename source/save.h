// re2ds - save games: re2ds.sav on the SD card (libfat).
//
// The PS1 saves at a typewriter, using up an ink ribbon, into one of the
// memory card's slots; loading puts Leon back in that room. Here one file
// holds SAVE_SLOTS slots.
#pragma once

#include <stdint.h>

#include "script.h"

#define SAVE_SLOTS 12
#define BOX_SLOTS 64            // the item box (not used yet)
#define BGM_TABLE_BYTES 320     // 141 rooms x main, sub

typedef struct {
    uint8_t used;
    uint8_t stage, room, cam;
    uint16_t saves;             // how often this game was saved
    int16_t hp;
    int32_t x, y, z;
    int16_t dir;                // DS units
    int8_t equipped;
    uint8_t saved_once;
    InvSlot inventory[INV_SLOTS];
    InvSlot box[BOX_SLOTS];
    uint8_t locks[128];
    uint8_t script[SCRIPT_STATE_SIZE];
    // version 2: the room music table (sce_bgmtbl_set changes it)
    uint8_t bgm_saved;
    uint8_t bgm[BGM_TABLE_BYTES];
} SaveSlot;

int save_init(void);                     // mounts the SD card; 0 if there is none
int save_available(void);                // the card is there
int save_read(SaveSlot slots[SAVE_SLOTS]);         // 0 if there is no file yet
int save_write(const SaveSlot slots[SAVE_SLOTS]);  // 0 on failure
