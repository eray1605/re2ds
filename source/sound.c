// re2ds - sound banks converted from PS1 VABs.
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "music.h"
#include "sound.h"

void sound_init(void)
{
    // calico's ARM9 side of the sound driver: libnds' soundPlaySample called it
    // on its first use (_soundEnsureInit); the channels are started directly
    // now, and without it the first sound command waited forever
    soundInit();
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

// The DS's 16 channels: 0-12 are the music's (music.c), 13-14 the effects',
// 15 the voice clips' and the movies' (the effects' too while no clip plays).
// The channels are started directly (soundPreparePcm): soundPlaySample asks
// the ARM7 which channels are free, and the music thread asked the same at
// the same time - both could take one channel. An effect with none free
// takes the one of its own that ends first; it took a music note before.
#define FX_FIRST 13
#define FX_VOICE 15
static u64 fx_end[16];          // tickGetCount() when the effect there is over

static int voice_on_channel(void);

int sound_channel_for(int voice, int frames)
{
    u64 now = tickGetCount();
    if (voice) {
        fx_end[FX_VOICE] = now;
        soundKill(FX_VOICE);
        return FX_VOICE;
    }
    int last = voice_on_channel() ? FX_VOICE - 1 : FX_VOICE, best = FX_FIRST;
    for (int ch = FX_FIRST; ch <= last; ch++) {
        if (fx_end[ch] <= now)
            return ch;
        if (fx_end[ch] < fx_end[best])
            best = ch;
    }
    // its own all busy (several zombies moaning and stepping, Leon's steps and
    // shots): a channel the music does not use, lent for the effect's length
    int lent = music_lend(frames);
    if (lent >= 0)
        return lent;
    soundKill(best);
    return best;
}

int sound_channel(int voice) { return sound_channel_for(voice, 60); }

int sound_play(const void *bank, int note, int volume, int pan)
{
    const SoundEntry *e = find(bank, note);
    if (!e || e->size < 4 || !e->rate)
        return -1;
    int eight = ((const SoundBankHeader *)bank)->reserved == 1;
    unsigned words = (e->size & ~3u) / 4;
    int frames = (int)((uint64_t)words * 4 / (eight ? 1 : 2) * 60 / e->rate) + 2;
    int ch = sound_channel_for(0, frames);
    soundPreparePcm((unsigned)ch | SOUND_START, (unsigned)(volume > 127 ? 127 : volume < 0 ? 0 : volume) << 4,
                    (unsigned)(pan > 127 ? 127 : pan < 0 ? 0 : pan), soundTimerFromHz(e->rate), SoundMode_OneShot,
                    eight ? SoundFmt_Pcm8 : SoundFmt_Pcm16, (const uint8_t *)bank + e->offset, 0, words);
    if (ch >= FX_FIRST)
        fx_end[ch] = tickGetCount() + (u64)(words * 4 / (eight ? 1 : 2)) * TICK_FREQ / e->rate + ticksFromHz(60);
    return ch;
}

int sound_frames(const void *bank, int note)
{
    const SoundEntry *e = find(bank, note);
    int bytes = ((const SoundBankHeader *)bank)->reserved == 1 ? 1 : 2;
    return e ? (int)(e->size / bytes * 60 / e->rate) : 0;
}

// ---------------------------------------------------------------- voices
#define VOICE_MAGIC 0x31435652 // "RVC1": u32 rate, u32 samples, IMA-ADPCM
static uint32_t *voice_data;
static int voice_channel = -1, voice_left, voice_frames;
// tickGetCount() a frame before the clip ends. A clip that has played out
// left its channel to the DS, which may have given it to a music note since:
// voice_stop kills the channel only before then (it killed it two frames
// after the end, and with it whatever note had started there).
static u64 voice_end;
static char voice_name[12];     // the clip's file name: its subtitle key

void voice_stop(void)
{
    if (voice_channel >= 0 && tickGetCount() < voice_end)
        soundKill(voice_channel);
    voice_channel = -1;
    voice_left = 0;
    voice_name[0] = 0;
    asset_free(voice_data);
    voice_data = NULL;
}

static int voice_vol = 120; // the PS1's scale: 120 at a room's start

static int voice_ds_volume(void)
{
    int v = voice_vol * 127 / 120;
    return v > 127 ? 127 : v;
}

void voice_volume(int v)
{
    if (v < 0 || v > 127)
        return; // the PS1 keeps the old one (values >= 128)
    voice_vol = v;
    if (voice_channel >= 0 && tickGetCount() < voice_end)
        soundSetVolume(voice_channel, voice_ds_volume());
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
    voice_channel = sound_channel(1);
    soundPreparePcm((unsigned)voice_channel | SOUND_START, (unsigned)voice_ds_volume() << 4, 64, soundTimerFromHz(rate),
                    SoundMode_OneShot, SoundFmt_ImaAdpcm, voice_data + 3, 0, ((size - 12) & ~3u) / 4);
    voice_left = (int)((uint64_t)samples * 60 / rate) + 2;
    voice_end = tickGetCount() + (u64)samples * TICK_FREQ / rate - ticksFromHz(60);
    voice_frames = 0;
    strcpy(voice_name, strrchr(path, '/') + 1);
    *strrchr(voice_name, '.') = 0;
}

const char *voice_key(void) { return voice_left > 0 ? voice_name : NULL; }
uint32_t voice_ms(void) { return (uint32_t)voice_frames * 1000 / 60; }

// by the clock, not by voice_frame's count: a main loop that took two frames a
// pass kept the music ducked twice as long
int voice_playing(void) { return voice_left > 0 && tickGetCount() < voice_end + ticksFromHz(60); }

static int voice_on_channel(void) { return voice_channel >= 0 && tickGetCount() < voice_end + ticksFromHz(60); }

void voice_frame(void)
{
    if (voice_left > 0)
        voice_frames++;
    if (voice_left > 0 && --voice_left == 0)
        voice_stop();
}
