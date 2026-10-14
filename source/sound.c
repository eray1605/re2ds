// re2ds - sound banks converted from PS1 VABs.
#include <nds.h>
#include <stdlib.h>

#include "assets.h"
#include "sound.h"

void sound_init(void)
{
    soundEnable();
}

void *sound_bank_load(const char *path)
{
    uint32_t size;
    SoundBankHeader *bank = asset_load(path, &size);
    if (!bank)
        return NULL;
    if (bank->magic != SOUND_MAGIC) {
        free(bank);
        return NULL;
    }
    DC_FlushRange(bank, size); // the ARM7 reads the samples from RAM
    return bank;
}

void sound_bank_free(void *bank)
{
    free(bank);
}

static const SoundEntry *find(const void *bank, int note)
{
    const SoundBankHeader *h = bank;
    if (!h)
        return NULL;
    const SoundEntry *e = (const SoundEntry *)(h + 1);
    for (int i = 0; i < h->count; i++)
        if (note >= e[i].note_lo && note <= e[i].note_hi && e[i].size)
            return &e[i];
    return NULL;
}

int sound_play(const void *bank, int note, int volume, int pan)
{
    const SoundEntry *e = find(bank, note);
    if (!e)
        return -1;
    return soundPlaySample((const uint8_t *)bank + e->offset, SoundFormat_16Bit, e->size & ~3,
                           e->rate, volume, pan, false, 0);
}

int sound_frames(const void *bank, int note)
{
    const SoundEntry *e = find(bank, note);
    return e ? (int)(e->size / 2 * 60 / e->rate) : 0;
}
