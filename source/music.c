// re2ds - background music (see music.h).
//
// PS1 SEQ (libsnd): "pQES", version, u16 ticks per quarter (big endian),
// 24-bit tempo in microseconds per quarter, 2 bytes time signature, then MIDI
// events with delta times and running status. Loops are NRPN controller 99:
// value 20 marks the loop start, 30 jumps back to it.
//
// Three sequences play at once, as on the PS1 (FUN_80055ed8, FUN_8005702c):
// the room's MAINxx and the two of its SUB_xx (sce_bgm_control id 0, 1, 2).
// A SUB file brings its own tones; a program it lacks is taken from the
// MAIN's bank: SUB_00, the fight at Kendo's, plays programs 5-10, which only
// MAIN01 (that room's music) has.
#include <nds.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "assets.h"
#include "music.h"

#define MUSIC_MAGIC 0x31554D52  // "RMU1": MAINxx, one sequence
#define MUSIC_MAGIC2 0x32554D52 // "RMU2": SUB_xx, two sequences
#define MAX_VOICES 12           // DS channels for music (the police station pieces hold up to
                                // 14 notes); the rest stay free for effects and voices
#define MASTER_VOLUME 64        // below the effects, like the PS1 mix
#define DUCKED 40               // while a voice clip plays
static int duck = 127;
#define FADE_FRAMES 90          // fade out (FUN_800755b8(seq, 0x7f, 0x5a))
#define PROGRAMS 16             // programs whose volume and pan sce_bgm_control sets

typedef struct {
    uint8_t program, lo, hi, centre;
    int8_t fine;
    uint8_t volume, pan, release;
    uint32_t offset, size, loop;
    uint16_t adsr1, adsr2;      // the VAB tone's SPU envelope (0, 0: none, held at full)
} MusicTone;

// A loaded file: its tones and samples, and the volume and pan sce_bgm_control
// gives the whole bank and each program (the VAB header's and program attributes')
typedef struct {
    uint8_t *data;
    const MusicTone *tones;
    int num_tones;
    int format, bytes;          // SoundFormat_*, bytes per sample
    uint8_t vol, pan, prog_vol[PROGRAMS], prog_pan[PROGRAMS];
} Bank;

enum { P_MAIN, P_SUB1, P_SUB2, PLAYERS };
// S_WAIT: loaded, waits for sce_bgm_control to start it; S_STOP: stopped,
// a start plays it from the beginning
enum { S_NONE, S_WAIT, S_PLAY, S_PAUSE, S_STOP };

typedef struct {
    Bank *bank;
    const uint8_t *seq, *seq_end, *pos, *loop_pos;
    uint8_t status, loop_status;
    uint32_t ticks_per_frame;   // 16.16
    uint32_t tick_acc;          // 16.16 ticks to run
    int32_t wait_ticks;
    uint16_t resolution;
    uint8_t program[16], ch_volume[16], ch_pan[16];
    int16_t ch_bend[16];
    int state;
    int fade;                   // frames left fading out, 0 = none
} Player;

typedef struct {
    int ch;                     // DS channel, -1 = free
    const Player *pl;
    const Bank *bank;           // the bank the tone lies in
    const MusicTone *tone;
    uint8_t midi, note, velocity;
    uint8_t phase;              // ENV_*: where in its envelope
    int32_t level;              // SPU envelope level, 0..0x7FFF
    uint8_t volume;             // before the envelope (voice_mix)
    uint8_t sent;               // the DS volume last set
    uint16_t frames;            // frames left for samples that play once (0 = loops)
    uint32_t start;             // when it started (the oldest held note gives way)
} Voice;

static Bank main_bank, sub_bank;
static Player players[PLAYERS];
static Voice voices[MAX_VOICES];
static uint32_t note_count;
static int current_main = -1, current_sub = -1;

static void set_tempo(Player *p, uint32_t us_per_quarter)
{
    if (!us_per_quarter)
        us_per_quarter = 500000;
    // ticks per second = resolution * 1e6 / tempo; per frame / 60
    p->ticks_per_frame = (uint32_t)(((uint64_t)p->resolution * 1000000ull << 16) / us_per_quarter / 60);
}

static uint32_t varlen(Player *p)
{
    uint32_t v = 0;
    while (p->pos < p->seq_end) {
        uint8_t b = *p->pos++;
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

// The SPU's ADSR envelope (psx-spx "SPU Volume and ADSR Generator"), which
// libsnd sets from the VAB tone: attack up to full, decay (exponential) down to
// the sustain level, sustain (rising or falling at its rate while the note is
// held: bells and plucks fade out on their own, e.g. the city's SUB_01, else a
// looped sample rang on like a siren), release after the note off. A rate
// gives a step every 2^max(0, shift - 11) samples of step << max(0, 11 - shift);
// summed here over the 735 samples of a 60 Hz frame.
enum { ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
#define ENV_MAX 0x7FFF
#define ENV_SILENT 0x100            // 1/128 of full: under one step of the DS volume

static int32_t env_delta(int shift, int step, int exp_dec, int exp_inc, int32_t level)
{
    int64_t per = (int64_t)step * ((int64_t)1 << (shift < 11 ? 11 - shift : 0));
    int64_t cycles = (int64_t)1 << (shift > 11 ? shift - 11 : 0);
    if (exp_inc && level > 0x6000)
        cycles *= 4;
    if (exp_dec)
        per = per * level / 0x8000;
    return (int32_t)(per * 735 / cycles);
}

// One frame of the envelope; 0 when the voice has died away.
static int envelope_step(Voice *v)
{
    const MusicTone *t = v->tone;
    int a1 = t->adsr1, a2 = t->adsr2;
    if (!a1 && !a2) // converted without the envelope: held, a short release
        a1 = 0x80FF, a2 = 0x5FCB;
    int32_t l = v->level;
    switch (v->phase) {
    case ENV_ATTACK: {
        int rate = (a1 >> 8) & 0x7F;
        l += env_delta(rate >> 2, 7 - (rate & 3), 0, a1 >> 15, l);
        if (l >= ENV_MAX) {
            l = ENV_MAX;
            v->phase = ENV_DECAY;
        }
        break;
    }
    case ENV_DECAY: {
        int32_t target = ((a1 & 15) + 1) * 0x800;
        if (l > target)
            l += env_delta(((a1 >> 4) & 15) * 4, -8, 1, 0, l);
        if (l <= target) {
            l = target > ENV_MAX ? ENV_MAX : target;
            v->phase = ENV_SUSTAIN;
        }
        break;
    }
    case ENV_SUSTAIN: {
        int rate = (a2 >> 6) & 0x7F, dec = (a2 >> 14) & 1;
        if (rate != 0x7F)
            l += env_delta(rate >> 2, dec ? -8 + (rate & 3) : 7 - (rate & 3), dec && (a2 >> 15), !dec && (a2 >> 15), l);
        break;
    }
    case ENV_RELEASE:
        l += env_delta(a2 & 0x1F, -8, (a2 >> 5) & 1, 0, l);
        break;
    }
    // an exponential fade only nears 0: below what the DS volume can still
    // play, the voice is done and frees its channel
    int fading = v->phase == ENV_RELEASE || (v->phase == ENV_SUSTAIN && ((a2 >> 14) & 1) && ((a2 >> 6) & 0x7F) != 0x7F);
    if (fading && l < ENV_SILENT)
        l = 0;
    v->level = l < 0 ? 0 : l > ENV_MAX ? ENV_MAX : l;
    return v->level > 0 || v->phase == ENV_ATTACK;
}

static void voice_send(Voice *v)
{
    int volume = v->volume * v->level / ENV_MAX;
    if (volume != v->sent) {
        soundSetVolume(v->ch, volume);
        v->sent = volume;
    }
}

// A voice's volume and pan as libsnd mixes them: tone, channel volume (cc 7)
// and pan (cc 10), velocity, program and whole bank (sce_bgm_control).
static void voice_mix(const Voice *v, int *volume, int *pan)
{
    const MusicTone *t = v->tone;
    const Bank *b = v->bank;
    int vol = b->vol, bgm_pan = b->pan - 64;
    if (t->program < PROGRAMS) {
        vol = vol * b->prog_vol[t->program] / 127;
        bgm_pan += b->prog_pan[t->program] - 64;
    }
    int x = t->volume * v->pl->ch_volume[v->midi] / 127 * v->velocity / 127 * MASTER_VOLUME / 127 * duck / 127;
    x = x * vol / 127;
    if (v->pl->fade)
        x = x * v->pl->fade / FADE_FRAMES;
    *volume = x;
    int p = v->pl->ch_pan[v->midi] + (t->pan - 64) + bgm_pan;
    *pan = p < 0 ? 0 : p > 127 ? 127 : p;
}

// libsnd applies volume and pan changes to the notes already sounding
// (piece 07 swells its held chords with 246 volume changes). pl NULL: all
// players; midi -1: all channels.
static void voices_update(const Player *pl, int midi)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch < 0 || (pl && v->pl != pl) || (midi >= 0 && v->midi != midi))
            continue;
        int volume, pan;
        voice_mix(v, &volume, &pan);
        v->volume = volume;
        voice_send(v);
        soundSetPan(v->ch, pan);
    }
}

void music_duck(int on)
{
    int d = on ? DUCKED : 127;
    if (d != duck) {
        duck = d;
        voices_update(NULL, -1);
    }
}

static void player_silence(const Player *pl)
{
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].pl == pl)
            voice_stop(&voices[i]);
}

// back to the beginning of its sequence
static void player_rewind(Player *p)
{
    p->resolution = p->seq[8] << 8 | p->seq[9];
    set_tempo(p, p->seq[10] << 16 | p->seq[11] << 8 | p->seq[12]);
    p->pos = p->loop_pos = p->seq + 15;
    p->status = p->loop_status = 0;
    p->tick_acc = 0;
    p->fade = 0;
    p->wait_ticks = varlen(p);
    for (int c = 0; c < 16; c++) {
        p->program[c] = 0;
        p->ch_volume[c] = 100;
        p->ch_pan[c] = 64;
        p->ch_bend[c] = 0;
    }
}

static void player_set(Player *p, Bank *bank, uint32_t off, uint32_t size, int state)
{
    player_silence(p);
    p->bank = bank;
    p->seq = bank->data + off;
    p->seq_end = p->seq + size;
    p->state = size >= 16 ? state : S_NONE;
    if (p->state != S_NONE)
        player_rewind(p);
}

static void player_clear(Player *p)
{
    player_silence(p);
    p->seq = NULL;
    p->state = S_NONE;
    p->fade = 0;
}

static void bank_free(Bank *b)
{
    for (int i = 0; i < PLAYERS; i++)
        if (players[i].bank == b)
            player_clear(&players[i]);
    for (int i = 0; i < MAX_VOICES; i++)
        if (voices[i].bank == b)
            voice_stop(&voices[i]);
    asset_free(b->data);
    b->data = NULL;
    b->num_tones = 0;
}

// music/<name>.bin (tools/bgm2ds.py): RMU1 / RMU2 header, then the tones
static int bank_load(Bank *b, const char *path, uint32_t magic, uint32_t head)
{
    static int voices_ready;
    if (!voices_ready) {
        for (int i = 0; i < MAX_VOICES; i++)
            voices[i].ch = -1;
        voices_ready = 1;
    }
    bank_free(b);
    b->vol = 127;
    b->pan = 64;
    memset(b->prog_vol, 127, sizeof(b->prog_vol));
    memset(b->prog_pan, 64, sizeof(b->prog_pan));
    uint32_t size;
    b->data = asset_load(path, &size);
    if (!b->data || size < head || *(uint32_t *)b->data != magic) {
        asset_free(b->data);
        b->data = NULL;
        return 0;
    }
    DC_FlushRange(b->data, size); // the ARM7 reads the samples from RAM
    b->num_tones = ((uint16_t *)b->data)[6];
    b->format = ((uint16_t *)b->data)[7] == 1 ? SoundFormat_8Bit : SoundFormat_16Bit;
    b->bytes = b->format == SoundFormat_8Bit ? 1 : 2;
    b->tones = (const MusicTone *)(b->data + head);
    return 1;
}

static void main_unload(void)
{
    bank_free(&main_bank);
    current_main = -1;
}

static void sub_unload(void)
{
    bank_free(&sub_bank);
    current_sub = -1;
}

void music_stop(void)
{
    sub_unload();
    main_unload();
}

int music_playing(void) { return players[P_MAIN].state == S_PLAY; }

// Hz for note on tone t, bend in 1/8192 of 2 semitones. The fine tune
// (VAB "shift", 1/128 semitone) raises the pitch (libsnd FUN_8007e3e4).
static uint32_t note_rate(const MusicTone *t, int note, int bend)
{
    float semis = (float)(note - t->centre) + t->fine / 128.0f + bend * 2.0f / 8192.0f;
    float hz = 44100.0f * powf(2.0f, semis / 12.0f);
    return hz > 65535.0f ? 65535 : (uint32_t)hz;
}

static void note_off(const Player *p, int midi, int note)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch >= 0 && v->pl == p && v->midi == midi && v->note == note && v->phase != ENV_RELEASE)
            v->phase = ENV_RELEASE;
    }
}

static void play_tone(const Player *p, const Bank *b, const MusicTone *t, int midi, int note, int velocity)
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
            if ((pass || voices[i].phase == ENV_RELEASE) && (!v || voices[i].start < v->start))
                v = &voices[i];
    voice_stop(v);
    v->pl = p;
    v->bank = b;
    v->tone = t;
    v->midi = midi;
    v->note = note;
    v->velocity = velocity;
    v->start = note_count++;
    int volume, pan;
    voice_mix(v, &volume, &pan);
    v->volume = volume;
    v->phase = ENV_ATTACK;
    v->level = 0;
    envelope_step(v); // its first frame: most attacks are there at once
    v->sent = volume * v->level / ENV_MAX;
    uint32_t rate = note_rate(t, note, p->ch_bend[midi]);
    int looped = t->loop != 0xFFFFFFFF;
    v->ch = soundPlaySample(b->data + t->offset, b->format, t->size & ~3, rate, v->sent, pan,
                            looped, looped ? t->loop / 4 : 0);
    v->frames = looped ? 0 : (uint16_t)(t->size / b->bytes * 60 / (rate ? rate : 1) + 1);
}

static int has_program(const Bank *b, int program)
{
    for (int i = 0; i < b->num_tones; i++)
        if (b->tones[i].program == program)
            return 1;
    return 0;
}

// libsnd plays every tone of the program whose key range holds the note:
// 24 of the 52 pieces layer two (e.g. a left / right pair, slightly detuned)
static void note_on(const Player *p, int midi, int note, int velocity)
{
    int prog = p->program[midi];
    const Bank *b = p->bank;
    if (b != &main_bank && !has_program(b, prog) && main_bank.data)
        b = &main_bank; // a SUB playing the MAIN's instruments (see the top)
    for (int i = 0; i < b->num_tones; i++)
        if (b->tones[i].program == prog && note >= b->tones[i].lo && note <= b->tones[i].hi)
            play_tone(p, b, &b->tones[i], midi, note, velocity);
}

static void bend(const Player *p, int midi)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch >= 0 && v->pl == p && v->midi == midi)
            soundSetFreq(v->ch, note_rate(v->tone, v->note, p->ch_bend[midi]));
    }
}

// One event at pos; returns 0 at the end of the sequence.
static int event(Player *p)
{
    if (p->pos >= p->seq_end)
        return 0;
    if (*p->pos & 0x80)
        p->status = *p->pos++;
    const uint8_t *pos = p->pos;
    int c = p->status & 0x0F;
    switch (p->status & 0xF0) {
    case 0x80:
        note_off(p, c, pos[0]);
        p->pos += 2;
        break;
    case 0x90:
        if (pos[1])
            note_on(p, c, pos[0], pos[1]);
        else
            note_off(p, c, pos[0]);
        p->pos += 2;
        break;
    case 0xA0:
        p->pos += 2;
        break;
    case 0xB0: {
        int cc = pos[0], value = pos[1];
        p->pos += 2;
        if (cc == 7 || cc == 10) {
            (cc == 7 ? p->ch_volume : p->ch_pan)[c] = value;
            voices_update(p, c);
        } else if (cc == 99 && value == 20) { // loop start
            p->loop_pos = p->pos;
            p->loop_status = p->status;
        } else if (cc == 99 && value == 30) { // loop end: back to the start
            p->pos = p->loop_pos;
            p->status = p->loop_status;
        }
        break;
    }
    case 0xC0:
        p->program[c] = *p->pos++;
        break;
    case 0xD0:
        p->pos++;
        break;
    case 0xE0:
        p->ch_bend[c] = (int16_t)((pos[0] | pos[1] << 7) - 8192);
        p->pos += 2;
        bend(p, c);
        break;
    case 0xF0:
        if (p->status == 0xFF) {
            int type = *p->pos++;
            if (type == 0x2F) { // end: start over at the loop point
                p->pos = p->loop_pos;
                p->status = p->loop_status;
                break;
            }
            if (type == 0x51) {
                set_tempo(p, p->pos[0] << 16 | p->pos[1] << 8 | p->pos[2]);
                p->pos += 3;
            }
        }
        break;
    }
    return 1;
}

static void player_frame(Player *p)
{
    if (p->state != S_PLAY)
        return;
    if (p->fade && --p->fade % 6 == 0) {
        if (!p->fade) { // faded out: stopped, a start plays it again
            player_silence(p);
            p->state = S_STOP;
            return;
        }
        voices_update(p, -1);
    }
    p->tick_acc += p->ticks_per_frame;
    int guard = 0;
    while (p->tick_acc >= (uint32_t)p->wait_ticks << 16 && guard++ < 256) {
        p->tick_acc -= (uint32_t)p->wait_ticks << 16;
        if (!event(p)) {
            p->pos = p->loop_pos;
            p->status = p->loop_status;
        }
        p->wait_ticks = varlen(p);
    }
}

// One 60 Hz step of the voices and sequences.
static void music_step(void)
{
    for (int i = 0; i < MAX_VOICES; i++) {
        Voice *v = &voices[i];
        if (v->ch < 0)
            continue;
        if (v->frames && --v->frames == 0) {
            v->ch = -1; // played out
            continue;
        }
        if (!envelope_step(v)) {
            voice_stop(v); // died away
            continue;
        }
        voice_send(v);
    }
    for (int i = 0; i < PLAYERS; i++)
        player_frame(&players[i]);
}

// The sequencer steps 60 times a second. main.c calls music_frame once per
// pass of its loop, but a pass takes two frames in a full room (Kendo, four
// zombies, Leon) and loading takes longer still: the music slowed down and
// stalled with its notes held. A 60 Hz tick task counts the frames that went
// by; music_frame catches up on them, after a long stall on a few only.
#define CATCH_UP 6
static TickTask clock_task;
static volatile uint32_t clock_frames;
static uint32_t frames_done;

static void clock_tick(TickTask *t)
{
    (void)t;
    clock_frames++;
}

void music_frame(void)
{
    static int started;
    if (!started) {
        started = 1;
        tickTaskStart(&clock_task, clock_tick, ticksFromHz(60), ticksFromHz(60));
        frames_done = clock_frames;
        return;
    }
    uint32_t due = clock_frames - frames_done;
    frames_done += due;
    if (due > CATCH_UP)
        due = CATCH_UP;
    while (due--)
        music_step();
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


// Room music (FUN_80055ed8). The main byte's bits 0-5 pick MAINxx, bits 6-7
// how it starts: 0 at once, else loaded and waiting for the room script's
// sce_bgm_control start. The same piece goes on across rooms; entered with a
// waiting mode it fades out instead. The sub byte's bits 0-5 pick SUB_xx (0xFF
// none), bit 6 / 7 make its first / second sequence wait (FUN_80056828, the
// others start at once, FUN_8005631c); a new main reloads it as well.
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
    int entry = table[3 + stages + 2 * index], sub_entry = table[4 + stages + 2 * index];
    int reloaded = 0;
    char path[40];
    Player *m = &players[P_MAIN];
    if (entry == 0xFF) {
        if (current_main >= 0)
            reloaded = 1;
        main_unload();
    } else if ((entry & 0x3F) == current_main && main_bank.data) {
        if (!(entry >> 6)) { // still waiting from the last room, or stopped: this one starts it
            if (m->state == S_STOP)
                player_rewind(m);
            if (m->state == S_WAIT || m->state == S_STOP)
                m->state = S_PLAY;
        } else if (m->state == S_PLAY && !m->fade) {
            m->fade = FADE_FRAMES;
        }
    } else {
        reloaded = 1;
        sub_unload(); // the PS1 frees the subs before it loads a main
        main_unload();
        snprintf(path, sizeof(path), "nitro:/music/main%02x.bin", entry & 0x3F);
        if (bank_load(&main_bank, path, MUSIC_MAGIC, 16)) {
            const uint32_t *h = (const uint32_t *)main_bank.data;
            player_set(m, &main_bank, h[1], h[2], (entry >> 6) ? S_WAIT : S_PLAY);
            current_main = entry & 0x3F;
        }
    }
    int sub = sub_entry == 0xFF ? -1 : sub_entry & 0x3F;
    if (sub != current_sub || reloaded) {
        sub_unload();
        if (sub < 0)
            return;
        snprintf(path, sizeof(path), "nitro:/music/sub_%02x.bin", sub);
        if (!bank_load(&sub_bank, path, MUSIC_MAGIC2, 24))
            return;
        const uint32_t *h = (const uint32_t *)sub_bank.data;
        player_set(&players[P_SUB1], &sub_bank, h[1], h[2], (sub_entry & 0x40) ? S_WAIT : S_PLAY);
        player_set(&players[P_SUB2], &sub_bank, h[4], h[5], (sub_entry & 0x80) ? S_WAIT : S_PLAY);
        current_sub = sub;
    } else {
        for (int i = 0; i < 2; i++) {
            Player *s = &players[P_SUB1 + i];
            if ((sub_entry & (0x40 << i)) && s->state == S_PLAY && !s->fade)
                s->fade = FADE_FRAMES;
        }
    }
}

// sce_bgm_control (opcode 0x51 -> FUN_8005702c): id 0 = the main music, 1 and
// 2 the sub's two sequences; operation 0 nop, 1 start, 2 stop, 3 resume, 4
// pause, 5 fade out (90 frames); type 0 = the whole bank (VAB header volume
// and pan), n = program n - 1 (the main's bank for id 0, else the sub's). The
// last two bytes are volume and pan, each stored minus 1; 0 leaves the value
// as it is (sce_bgm_control 0, 2, 0, 0, 0 only stops).
void music_control(int id, int op, int type, int volume, int pan)
{
    if (id < 0 || id >= PLAYERS)
        return;
    Player *p = &players[id];
    if (p->state != S_NONE) {
        if (op == 1 || op == 3) { // start / resume
            if (p->state == S_STOP)
                player_rewind(p);
            p->state = S_PLAY;
            p->fade = 0;
        } else if (op == 4 && p->state == S_PLAY) { // pause: silent, resumes where it was
            player_silence(p);
            p->state = S_PAUSE;
        } else if (op == 2) { // stopped, not gone: its bank stays, a start replays it
            player_silence(p);
            p->state = S_STOP;
        } else if (op == 5 && p->state == S_PLAY && !p->fade) {
            p->fade = FADE_FRAMES;
        }
    }
    Bank *b = id == P_MAIN ? &main_bank : &sub_bank;
    uint8_t *v = type == 0 ? &b->vol : type <= PROGRAMS ? &b->prog_vol[type - 1] : NULL;
    uint8_t *pn = type == 0 ? &b->pan : type <= PROGRAMS ? &b->prog_pan[type - 1] : NULL;
    if (v && volume)
        *v = volume - 1 > 127 ? 127 : volume - 1;
    if (pn && pan)
        *pn = pan - 1 > 127 ? 127 : pan - 1;
    if (volume || pan)
        voices_update(NULL, -1);
}
