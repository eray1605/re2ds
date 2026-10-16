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
#define MAX_VOICES 10           // DS channels for music; the rest stay free for effects
#define MASTER_VOLUME 64        // below the effects, like the PS1 mix
#define DUCKED 40               // while a voice clip plays
static int duck = 127;
#define RELEASE_FRAMES 10

typedef struct {
    uint8_t program, lo, hi, centre;
    int8_t fine;
    uint8_t volume, pan, release;
    uint32_t offset, size, loop, pad;
} MusicTone;

typedef struct {
    int ch;                     // DS channel, -1 = free
    uint8_t midi, note;
    int8_t release;             // frames left fading out, -1 = held
    uint8_t volume;
    uint16_t frames;            // frames left for samples that play once (0 = loops)
} Voice;

static uint8_t *data;
static const MusicTone *tones;
static int num_tones;
static const uint8_t *seq, *seq_end, *pos, *loop_pos;
static uint8_t status, loop_status;
static uint32_t ticks_per_frame;  // 16.16
static uint32_t tick_acc;         // 16.16 ticks to run
static int32_t wait_ticks;
static uint16_t resolution;
static uint8_t program[16], ch_volume[16], ch_pan[16];
static int16_t ch_bend[16];
static Voice voices[MAX_VOICES];
static int current_main = -1;
// sce_bgm_control volumes (left, right) of the whole music and of programs 0-2
static uint8_t main_l = 127, main_r = 127, prog_l[3] = { 127, 127, 127 }, prog_r[3] = { 127, 127, 127 };

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

void music_duck(int on) { duck = on ? DUCKED : 127; }

void music_stop(void)
{
    for (int i = 0; i < MAX_VOICES; i++)
        voice_stop(&voices[i]);
    asset_free(data);
    data = NULL;
    seq = NULL;
    current_main = -1;
}

int music_playing(void) { return seq != NULL; }

void music_play(const char *path)
{
    music_stop();
    main_l = main_r = 127;
    for (int i = 0; i < 3; i++)
        prog_l[i] = prog_r[i] = 127;
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

// Hz for note on tone t, bend in 1/8192 of 2 semitones
static uint32_t note_rate(const MusicTone *t, int note, int bend)
{
    float semis = (float)(note - t->centre) - t->fine / 128.0f + bend * 2.0f / 8192.0f;
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

static void note_on(int midi, int note, int velocity)
{
    const MusicTone *t = NULL;
    for (int i = 0; i < num_tones; i++)
        if (tones[i].program == program[midi] && note >= tones[i].lo && note <= tones[i].hi) {
            t = &tones[i];
            break;
        }
    if (!t || !t->size)
        return;
    // a free voice, else the oldest one fading out, else the first
    Voice *v = NULL;
    for (int i = 0; i < MAX_VOICES && !v; i++)
        if (voices[i].ch < 0)
            v = &voices[i];
    for (int i = 0; i < MAX_VOICES && !v; i++)
        if (voices[i].release >= 0)
            v = &voices[i];
    if (!v)
        v = &voices[0];
    voice_stop(v);
    int l = main_l, r = main_r;
    if (program[midi] < 3) {
        l = l * prog_l[program[midi]] / 127;
        r = r * prog_r[program[midi]] / 127;
    }
    int volume = t->volume * ch_volume[midi] / 127 * velocity / 127 * MASTER_VOLUME / 127 * duck / 127;
    volume = volume * (l > r ? l : r) / 127;
    int pan = ch_pan[midi] + (t->pan - 64) + (l + r ? (r - l) * 64 / (l + r) : 0);
    pan = pan < 0 ? 0 : pan > 127 ? 127 : pan;
    uint32_t rate = note_rate(t, note, ch_bend[midi]);
    int looped = t->loop != 0xFFFFFFFF;
    v->ch = soundPlaySample(data + t->offset, SoundFormat_16Bit, t->size & ~3, rate, volume, pan,
                            looped, looped ? t->loop / 4 : 0);
    v->midi = midi;
    v->note = note;
    v->release = -1;
    v->volume = volume;
    v->frames = looped ? 0 : (uint16_t)(t->size / 2 * 60 / (rate ? rate : 1) + 1);
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
        if (cc == 7)
            ch_volume[c] = value;
        else if (cc == 10)
            ch_pan[c] = value;
        else if (cc == 99 && value == 20) { // loop start
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
    if (!seq)
        return;
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

void music_room(int stage, int room)
{
    static uint8_t *table;
    if (!table)
        table = asset_load("nitro:/music/bgm_table.bin", NULL);
    if (!table)
        return;
    int stages = table[0];
    if (stage < 1 || stage > stages)
        return;
    int rooms = table[1 + stages] | table[2 + stages] << 8;
    int index = table[stage] + room; // first room of the stage + room
    if (index >= rooms)
        return;
    int main = table[3 + stages + 2 * index];
    if (main == 0xFF) {
        music_stop();
        return;
    }
    main &= 0x3F;
    if (main == current_main && seq)
        return;
    char path[40];
    snprintf(path, sizeof(path), "nitro:/music/main%02x.bin", main);
    music_play(path);
    current_main = main;
}

// sce_bgm_control (opcode 0x51): id 0 = main music; operation 0 nop, 1 start,
// 2 stop, 3 restart, 4 pause, 5 fade out; type 0 = main volume, 1-3 = volume
// of programs 0-2 (left, right)
void music_control(int id, int op, int type, int left, int right)
{
    if (id != 0)
        return; // the sub music (SUB_xx) is not played yet
    if (type == 0) {
        main_l = left;
        main_r = right;
    } else if (type <= 3) {
        prog_l[type - 1] = left;
        prog_r[type - 1] = right;
    }
    if (op == 2 || op == 4 || op == 5) {
        int keep = current_main;
        music_stop();
        current_main = keep; // the room still "has" it: no restart on re-entry
    }
}
