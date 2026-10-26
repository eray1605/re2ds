// re2ds - background music (see music.h).
//
// PS1 SEQ (libsnd): "pQES", version, u16 ticks per quarter (big endian),
// 24-bit tempo in microseconds per quarter, 2 bytes time signature, then MIDI
// events with delta times and running status. Loops are NRPN controller 99:
// value 20 marks the loop start, 30 jumps back to it.
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "music.h"

#define MUSIC_MAGIC 0x31554D52 // "RMU1"
#define MAX_VOICES 12           // DS channels for music (the police station pieces hold up to
                                // 14 notes); the rest stay free for effects and voices
#define MASTER_VOLUME 64        // below the effects, like the PS1 mix
#define DUCKED 40               // while a voice clip plays
static int duck = 127;
#define RELEASE_FRAMES 10
#define FADE_FRAMES 90          // fade out (FUN_800755b8(seq, 0x7f, 0x5a))
#define PROGRAMS 16             // programs whose volume and pan sce_bgm_control sets

typedef struct {
    uint8_t program, lo, hi, centre;
    int8_t fine;
    uint8_t volume, pan, release;
    uint32_t offset, size, loop, pad;
} MusicTone;

typedef struct {
    int ch;                     // DS channel, -1 = free
    const MusicTone *tone;
    uint8_t midi, note, velocity;
    int8_t release;             // frames left fading out, -1 = held
    uint8_t volume;
    uint16_t frames;            // frames left for samples that play once (0 = loops)
    uint32_t start;             // when it started (the oldest held note gives way)
} Voice;

static uint8_t *data;
static const MusicTone *tones;
static int num_tones;
static int sample_format, sample_bytes; // format field of the file: 1 = 8-bit (else 16-bit)
static const uint8_t *seq, *seq_end, *pos, *loop_pos;
static uint8_t status, loop_status;
static uint32_t ticks_per_frame;  // 16.16
static uint32_t tick_acc;         // 16.16 ticks to run
static int32_t wait_ticks;
static uint16_t resolution;
static uint8_t program[16], ch_volume[16], ch_pan[16];
static int16_t ch_bend[16];
static Voice voices[MAX_VOICES];
static uint32_t note_count;
static int current_main = -1;
static int paused;              // loaded, waiting for sce_bgm_control to start it
static int fade;                // frames left fading out, 0 = none
// sce_bgm_control (FUN_8005702c): volume and pan (64 = centre) of the whole
// music and of each program (the VAB header's and program attributes')
static uint8_t main_vol = 127, main_pan = 64, prog_vol[PROGRAMS], prog_pan[PROGRAMS];

static void set_tempo(uint32_t us_per_quarter)
{
    if (!us_per_quarter)
        us_per_quarter = 500000;
    // ticks per second = resolution * 1e6 / tempo; per frame / 60
    ticks_per_frame = (uint32_t)(((uint64_t)resolution * 1000000ull << 16) / us_per_quarter / 60);
}

static uint32_t varlen(void)
{
    uint32_t v = 0;
    while (pos < seq_end) {
        uint8_t b = *pos++;
        v = v << 7 | (b & 0x7F);
        if (!(b & 0x80))
            break;
    }
    return v;
}

static void voice_stop(Voice *v)
{
    if (v->ch >= 0)
        soundKill(v->ch);
    v->ch = -1;
}

// A voice's volume and pan as libsnd mixes them: tone, channel volume (cc 7)
// and pan (cc 10), velocity, program and whole music (sce_bgm_control).
static void voice_mix(const Voice *v, int *volume, int *pan)
{
    const MusicTone *t = v->tone;
    int vol = main_vol, bgm_pan = main_pan - 64;
    if (t->program < PROGRAMS) {
        vol = vol * prog_vol[t->program] / 127;
        bgm_pan += prog_pan[t->program] - 64;
    }
    int x = t->volume * ch_volume[v->midi] / 127 * v->velocity / 127 * MASTER_VOLUME / 127 * duck / 127;
    x = x * vol / 127;
    if (fade)
        x = x * fade / FADE_FRAMES;
    *volume = x;
    int p = ch_pan[v->midi] + (t->pan - 64) + bgm_pan;
    *pan = p < 0 ? 0 : p > 127 ? 127 : p;
}

// libsnd applies volume and pan changes to the notes already sounding
// (piece 07 swells its held chords with 246 volume changes)
static void voices_update(int midi)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch < 0 || (midi >= 0 && v->midi != midi))
            continue;
        int volume, pan;
        voice_mix(v, &volume, &pan);
        v->volume = volume;
        if (v->release < 0)
            soundSetVolume(v->ch, volume);
        soundSetPan(v->ch, pan);
    }
}

void music_duck(int on)
{
    int d = on ? DUCKED : 127;
    if (d != duck) {
        duck = d;
        voices_update(-1);
    }
}

void music_stop(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        voice_stop(&voices[i]);
    asset_free(data);
    data = NULL;
    seq = NULL;
    current_main = -1;
    paused = 0;
    fade = 0;
}

int music_playing(void) { return seq != NULL; }

void music_play(const char *path)
{
    music_stop();
    main_vol = 127;
    main_pan = 64;
    memset(prog_vol, 127, sizeof(prog_vol));
    memset(prog_pan, 64, sizeof(prog_pan));
    uint32_t size;
    data = asset_load(path, &size);
    if (!data || *(uint32_t *)data != MUSIC_MAGIC || size < 16) {
        asset_free(data);
        data = NULL;
        return;
    }
    DC_FlushRange(data, size); // the ARM7 reads the samples from RAM
    uint32_t seq_off = ((uint32_t *)data)[1], seq_size = ((uint32_t *)data)[2];
    num_tones = ((uint16_t *)data)[6];
    sample_format = ((uint16_t *)data)[7] == 1 ? SoundFormat_8Bit : SoundFormat_16Bit;
    sample_bytes = sample_format == SoundFormat_8Bit ? 1 : 2;
    tones = (const MusicTone *)(data + 16);
    seq = data + seq_off;
    seq_end = seq + seq_size;
    resolution = seq[8] << 8 | seq[9];
    set_tempo(seq[10] << 16 | seq[11] << 8 | seq[12]);
    pos = loop_pos = seq + 15;
    status = loop_status = 0;
    tick_acc = 0;
    wait_ticks = varlen();
    for (int c = 0; c < 16; c++) {
        program[c] = 0;
        ch_volume[c] = 100;
        ch_pan[c] = 64;
        ch_bend[c] = 0;
    }
    for (int i = 0; i < MAX_VOICES; i++)
        voices[i].ch = -1;
}

// Hz for note on tone t, bend in 1/8192 of 2 semitones. The fine tune
// (VAB "shift", 1/128 semitone) raises the pitch (libsnd FUN_8007e3e4).
static uint32_t note_rate(const MusicTone *t, int note, int bend)
{
    float semis = (float)(note - t->centre) + t->fine / 128.0f + bend * 2.0f / 8192.0f;
    float hz = 44100.0f * powf(2.0f, semis / 12.0f);
    return hz > 65535.0f ? 65535 : (uint32_t)hz;
}

static void note_off(int midi, int note)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch >= 0 && v->midi == midi && v->note == note && v->release < 0)
            v->release = RELEASE_FRAMES;
    }
}

static void play_tone(const MusicTone *t, int midi, int note, int velocity)
{
    if (!t->size)
        return;
    // a free voice, else the oldest one fading out, else the oldest held one
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES && !v; i++)
        if (voices[i].ch < 0)
            v = &voices[i];
    for (int pass = 0; pass < 2 && !v; pass++)
        for (int i = 0; i < MAX_VOICES; i++)
            if ((pass || voices[i].release >= 0) && (!v || voices[i].start < v->start))
                v = &voices[i];
    voice_stop(v);
    v->tone = t;
    v->midi = midi;
    v->note = note;
    v->velocity = velocity;
    v->start = note_count++;
    int volume, pan;
    voice_mix(v, &volume, &pan);
    uint32_t rate = note_rate(t, note, ch_bend[midi]);
    int looped = t->loop != 0xFFFFFFFF;
    v->ch = soundPlaySample(data + t->offset, sample_format, t->size & ~3, rate, volume, pan,
                            looped, looped ? t->loop / 4 : 0);
    v->release = -1;
    v->volume = volume;
    v->frames = looped ? 0 : (uint16_t)(t->size / sample_bytes * 60 / (rate ? rate : 1) + 1);
}

// libsnd plays every tone of the program whose key range holds the note:
// 24 of the 52 pieces layer two (e.g. a left / right pair, slightly detuned)
static void note_on(int midi, int note, int velocity)
{
    for (int i = 0; i < num_tones; i++)
        if (tones[i].program == program[midi] && note >= tones[i].lo && note <= tones[i].hi)
            play_tone(&tones[i], midi, note, velocity);
}

static void bend(int midi)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch < 0 || v->midi != midi)
            continue;
        for (int k = 0; k < num_tones; k++)
            if (tones[k].program == program[midi] && v->note >= tones[k].lo && v->note <= tones[k].hi) {
                soundSetFreq(v->ch, note_rate(&tones[k], v->note, ch_bend[midi]));
                break;
            }
    }
}

// One event at pos; returns 0 at the end of the sequence.
static int event(void)
{
    if (pos >= seq_end)
        return 0;
    if (*pos & 0x80)
        status = *pos++;
    int c = status & 0x0F;
    switch (status & 0xF0) {
    case 0x80:
        note_off(c, pos[0]);
        pos += 2;
        break;
    case 0x90:
        if (pos[1])
            note_on(c, pos[0], pos[1]);
        else
            note_off(c, pos[0]);
        pos += 2;
        break;
    case 0xA0:
        pos += 2;
        break;
    case 0xB0: {
        int cc = pos[0], value = pos[1];
        pos += 2;
        if (cc == 7 || cc == 10) {
            (cc == 7 ? ch_volume : ch_pan)[c] = value;
            voices_update(c);
        } else if (cc == 99 && value == 20) { // loop start
            loop_pos = pos;
            loop_status = status;
        } else if (cc == 99 && value == 30) { // loop end: back to the start
            pos = loop_pos;
            status = loop_status;
        }
        break;
    }
    case 0xC0:
        program[c] = *pos++;
        break;
    case 0xD0:
        pos++;
        break;
    case 0xE0:
        ch_bend[c] = (int16_t)((pos[0] | pos[1] << 7) - 8192);
        pos += 2;
        bend(c);
        break;
    case 0xF0:
        if (status == 0xFF) {
            int type = *pos++;
            if (type == 0x2F) { // end: start over at the loop point
                pos = loop_pos;
                status = loop_status;
                break;
            }
            if (type == 0x51) {
                set_tempo(pos[0] << 16 | pos[1] << 8 | pos[2]);
                pos += 3;
            }
        }
        break;
    }
    return 1;
}

void music_frame(void)
{
    if (!seq || paused)
        return;
    if (fade && --fade % 6 == 0) {
        if (!fade) {
            int keep = current_main;
            music_stop();
            current_main = keep; // stopped, not gone: sce_bgm_control can start it again
            return;
        }
        voices_update(-1);
    }
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch < 0)
            continue;
        if (v->frames && --v->frames == 0) {
            v->ch = -1; // played out
            continue;
        }
        if (v->release > 0) {
            v->release--;
            soundSetVolume(v->ch, v->volume * v->release / RELEASE_FRAMES);
            if (!v->release)
                voice_stop(v);
        }
    }
    tick_acc += ticks_per_frame;
    int guard = 0;
    while (tick_acc >= (uint32_t)wait_ticks << 16 && guard++ < 256) {
        tick_acc -= (uint32_t)wait_ticks << 16;
        if (!event()) {
            pos = loop_pos;
            status = loop_status;
        }
        wait_ticks = varlen();
    }
}

// bgm_table.bin: u8 stages, u8 first room[stages], u16 rooms, then main, sub
// per room. Kept in RAM: sce_bgmtbl_set changes it and saves keep it, as the
// PS1's copy at 0x800cc2cc is part of its save state.
static uint8_t *table;

static int table_load(void)
{
    if (!table)
        table = asset_load("nitro:/music/bgm_table.bin", NULL);
    return table != NULL;
}

static uint8_t *table_pairs(int *bytes)
{
    if (!table_load())
        return NULL;
    int stages = table[0];
    *bytes = 2 * (table[1 + stages] | table[2 + stages] << 8);
    return table + 3 + stages;
}

void music_table_reset(void)
{
    asset_free(table);
    table = NULL;
    table_load();
}

int music_table_get(uint8_t *out, int max)
{
    int bytes;
    uint8_t *pairs = table_pairs(&bytes);
    if (!pairs || bytes > max)
        return 0;
    memcpy(out, pairs, bytes);
    return bytes;
}

void music_table_put(const uint8_t *in, int max)
{
    int bytes;
    uint8_t *pairs = table_pairs(&bytes);
    if (pairs && bytes <= max)
        memcpy(pairs, in, bytes);
}

// sce_bgmtbl_set (opcode 0x57 -> FUN_80057330)
void music_table_set(int stage, int room, int main, int sub)
{
    if (!table_load() || stage < 1 || stage > table[0])
        return;
    int stages = table[0];
    int index = table[stage] + room;
    if (index >= (table[1 + stages] | table[2 + stages] << 8))
        return;
    table[3 + stages + 2 * index] = main;
    table[4 + stages + 2 * index] = sub;
}

// Room music (FUN_80055ed8): the main byte's bits 0-5 pick MAINxx, bits 6-7
// how it starts: 0 at once, else loaded and waiting for the room script's
// sce_bgm_control start. The same piece goes on across rooms; entered with a
// waiting mode it fades out instead.
void music_room(int stage, int room)
{
    if (!table_load())
        return;
    int stages = table[0];
    if (stage < 1 || stage > stages)
        return;
    int rooms = table[1 + stages] | table[2 + stages] << 8;
    int index = table[stage] + room; // first room of the stage + room
    if (index >= rooms)
        return;
    int entry = table[3 + stages + 2 * index];
    if (entry == 0xFF) {
        music_stop();
        return;
    }
    int main = entry & 0x3F, wait = entry >> 6;
    if (main == current_main && seq) {
        if (!wait)
            paused = 0; // still waiting from the last room: this one starts it
        else if (!paused && !fade)
            fade = FADE_FRAMES;
        return;
    }
    char path[40];
    snprintf(path, sizeof(path), "nitro:/music/main%02x.bin", main);
    music_play(path);
    current_main = main;
    paused = wait;
}

// sce_bgm_control (opcode 0x51 -> FUN_8005702c): id 0 = main music; operation
// 0 nop, 1 start, 2 stop, 3 resume, 4 pause, 5 fade out (90 frames); type 0 =
// the whole music (VAB header volume and pan), n = program n - 1. The last two
// bytes are volume and pan, each stored minus 1; 0 leaves the value as it is
// (sce_bgm_control 0, 2, 0, 0, 0 only stops).
void music_control(int id, int op, int type, int volume, int pan)
{
    if (id != 0)
        return; // the sub music (SUB_xx) is not played yet
    if (op == 1 || op == 3) { // (re)start the room's music
        fade = 0;
        if (paused) {
            paused = 0;
        } else if (!seq && current_main >= 0) {
            uint8_t mv = main_vol, mp = main_pan, pv[PROGRAMS], pp[PROGRAMS];
            memcpy(pv, prog_vol, PROGRAMS);
            memcpy(pp, prog_pan, PROGRAMS);
            char path[40];
            int main = current_main;
            snprintf(path, sizeof(path), "nitro:/music/main%02x.bin", main);
            music_play(path);
            current_main = main;
            main_vol = mv;
            main_pan = mp;
            memcpy(prog_vol, pv, PROGRAMS);
            memcpy(prog_pan, pp, PROGRAMS);
        }
    } else if (op == 4 && seq) { // pause: silent, resumes where it was
        for (int i = 0; i < MAX_VOICES; i++)
            voice_stop(&voices[i]);
        paused = 1;
    } else if (op == 2) {
        int keep = current_main;
        music_stop();
        current_main = keep; // stopped, not gone: start plays it again
    } else if (op == 5 && seq && !fade) {
        fade = FADE_FRAMES;
    }
    uint8_t *v = type == 0 ? &main_vol : type <= PROGRAMS ? &prog_vol[type - 1] : NULL;
    uint8_t *p = type == 0 ? &main_pan : type <= PROGRAMS ? &prog_pan[type - 1] : NULL;
    if (v && volume)
        *v = volume - 1 > 127 ? 127 : volume - 1;
    if (p && pan)
        *p = pan - 1 > 127 ? 127 : pan - 1;
    if (volume || pan)
        voices_update(-1);
}
