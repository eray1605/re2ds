// re2ds - save games (see save.h).
//
// File: u32 "RSV1", u16 version, u16 slot count, u32 slot size, then the
// slots as SaveSlot. Written whole every time (about 30 KB).
#include <fat.h>
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "save.h"

#define SAVE_MAGIC 0x31565352  // "RSV1"
#define SAVE_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version, slots;
    uint32_t slot_size;
} SaveHeader;

// The DSi's SD card is "sd:", a flashcart's (DLDI) "fat:"
static const char *const paths[] = { "sd:/re2ds.sav", "fat:/re2ds.sav" };
static int mounted, path = -1;

int save_init(void)
{
    mounted = fatInitDefault();
    if (!mounted)
        return 0;
    // the first device we can write to
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "rb");
        if (f) {
            fclose(f);
            path = i;
            return 1;
        }
    }
    for (unsigned i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        FILE *f = fopen(paths[i], "ab");
        if (f) {
            fclose(f);
            remove(paths[i]); // created only to see that it works
            path = i;
            return 1;
        }
    }
    return 0;
}

int save_available(void) { return mounted && path >= 0; }

int save_read(SaveSlot slots[SAVE_SLOTS])
{
    memset(slots, 0, sizeof(SaveSlot) * SAVE_SLOTS);
    if (!save_available())
        return 0;
    FILE *f = fopen(paths[path], "rb");
    if (!f)
        return 0;
    SaveHeader h;
    int ok = fread(&h, sizeof(h), 1, f) == 1 && h.magic == SAVE_MAGIC && h.version == SAVE_VERSION &&
             h.slot_size == sizeof(SaveSlot) && h.slots == SAVE_SLOTS &&
             fread(slots, sizeof(SaveSlot), SAVE_SLOTS, f) == SAVE_SLOTS;
    fclose(f);
    if (!ok)
        memset(slots, 0, sizeof(SaveSlot) * SAVE_SLOTS);
    return ok;
}

int save_write(const SaveSlot slots[SAVE_SLOTS])
{
    if (!save_available())
        return 0;
    FILE *f = fopen(paths[path], "wb");
    if (!f)
        return 0;
    SaveHeader h = { SAVE_MAGIC, SAVE_VERSION, SAVE_SLOTS, sizeof(SaveSlot) };
    int ok = fwrite(&h, sizeof(h), 1, f) == 1 && fwrite(slots, sizeof(SaveSlot), SAVE_SLOTS, f) == SAVE_SLOTS;
    return fclose(f) == 0 && ok;
}
