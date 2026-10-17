// re2ds - sound banks converted from PS1 VABs.
#include <nds.h>
#include <stdio.h>
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

// ---------------------------------------------------------------- voices
#define VOICE_MAGIC 0x31435652 // "RVC1": u32 rate, u32 samples, IMA-ADPCM
static uint32_t *voice_data;
static int voice_channel = -1, voice_left;

void voice_stop(void)
{
    if (voice_channel >= 0)
        soundKill(voice_channel);
    voice_channel = -1;
    voice_left = 0;
    asset_free(voice_data);
    voice_data = NULL;
}

void voice_play(int stage, int clip)
{
    char path[40];
    uint32_t size;
    voice_stop();
    if (stage == 0) // the common clips (xa_on 1, n)
        snprintf(path, sizeof(path), "nitro:/voice/c_%02d.bin", clip);
    else
        snprintf(path, sizeof(path), "nitro:/voice/s%d_%02d.bin", stage, clip);
    voice_data = asset_load(path, &size);
    if (!voice_data || size < 16 || voice_data[0] != VOICE_MAGIC) {
        asset_free(voice_data);
        voice_data = NULL;
        return;
    }
    uint32_t rate = voice_data[1], samples = voice_data[2];
    DC_FlushRange(voice_data, size);
    voice_channel = soundPlaySample(voice_data + 3, SoundFormat_ADPCM, (size - 12) & ~3, rate, 127, 64,
                                    false, 0);
    voice_left = (int)((uint64_t)samples * 60 / rate) + 2;
}

int voice_playing(void) { return voice_left > 0; }

void voice_frame(void)
{
    if (voice_left > 0 && --voice_left == 0)
        voice_stop();
}
