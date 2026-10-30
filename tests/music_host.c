// Host test: the music sequencer (music.c) without the DS sound hardware.
// Plays a room's music for a while and reports how many notes sound at once,
// how often a note takes a channel from another one, and the events per
// frame.
// Build: cc -O1 -g -fsanitize=undefined -Itests/stubs -Isource tests/music_host.c -o build/music_host -lm
// Run:   ./build/music_host <stage> <room> [seconds] [op script: "frame:id:op,..."] [slow]
//        (slow: the main loop calls music_frame every second frame)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <nds.h>
enum { SoundFormat_16Bit = 1, SoundFormat_8Bit = 0 };
#define HOST_CHANNELS 16
static int ch_used[HOST_CHANNELS], ch_end[HOST_CHANNELS], host_frame, plays, kills, fails, vol_msgs;
static int soundPlaySample(const void *d, int fmt, uint32_t size, uint32_t rate, int vol, int pan, bool loop, uint16_t lp)
{
    (void)d; (void)vol; (void)pan; (void)lp;
    for (int i = 0; i < HOST_CHANNELS; i++)
        if (!ch_used[i]) {
            ch_used[i] = 1;
            // a sample played once frees its channel when it ends, as on the DS
            ch_end[i] = loop ? 0 : host_frame + (int)(size / (fmt == SoundFormat_8Bit ? 1 : 2) * 60 / (rate ? rate : 1)) + 1;
            plays++;
            return i;
        }
    fails++;
    return -1;
}
static void soundKill(int ch) { if (ch >= 0 && ch < HOST_CHANNELS) ch_used[ch] = 0; kills++; }
static void soundChSetVolume(unsigned ch, unsigned v) { (void)ch; (void)v; vol_msgs++; }
static void soundSetPan(int ch, int p) { (void)ch; (void)p; vol_msgs++; }
static void soundSetFreq(int ch, uint32_t f) { (void)ch; (void)f; vol_msgs++; }
void setBrightness(int screen, int level) { (void)screen; (void)level; }
typedef struct { int unused; } TickTask;
static void tickTaskStart(TickTask *t, void (*f)(TickTask *), int a, int b) { (void)t; (void)f; (void)a; (void)b; }
static int ticksFromHz(int hz) { return hz; }
static FILE *host_fopen(const char *path, const char *mode)
{
    const char *p = strstr(path, "nitro:/");
    char real[256];
    snprintf(real, sizeof(real), "nitrofiles/%s", p ? p + 7 : path);
    return fopen(real, mode);
}
#define fopen host_fopen
#include "../source/assets.c"
#undef fopen
#include "../source/music.c"

int main(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: music_host stage room [seconds] [frame:id:op,...]\n");
        return 1;
    }
    int stage = atoi(argv[1]), room = (int)strtol(argv[2], NULL, 16), secs = argc > 3 ? atoi(argv[3]) : 60;
    music_room(stage, room);
    printf("main %d (state %d), sub %d (states %d %d)\n", current_main, players[0].state, current_sub,
           players[1].state, players[2].state);
    int peak = 0, busy_frames = 0, peak_msgs = 0, loud_peak = 0, loud_max_voice = 0;
    long sum = 0, loud_sum = 0;
    int gain_min = 256, gain_low = 0; // the limiter (music.c mix_gain): lowest, frames below 3/4
    for (int f = 0; f < secs * 60; f++) {
        if (argc > 4) {
            char buf[256], *save = NULL;
            snprintf(buf, sizeof(buf), "%s", argv[4]);
            for (char *it = strtok_r(buf, ",", &save); it; it = strtok_r(NULL, ",", &save)) {
                int at, id, op;
                if (sscanf(it, "%d:%d:%d", &at, &id, &op) == 3 && at == f)
                    music_control(id, op, 0, 0, 0);
            }
        }
        int msgs = vol_msgs;
        host_frame = f;
        for (int i = 0; i < HOST_CHANNELS; i++)
            if (ch_used[i] && ch_end[i] && f >= ch_end[i])
                ch_used[i] = 0;
        clock_frames += (argc > 5 && f % 2) ? 0 : (argc > 5 ? 2 : 1); // optional: a loop pass every 2 frames
        music_frame();
        int n = 0, loud = 0;
        for (int i = 0; i < MAX_VOICES; i++)
            if (voices[i].ch >= 0) {
                n++;
                loud += voices[i].sent / 16; // what the DS mixer adds up (0..127 a channel)
                if (voices[i].sent / 16 > loud_max_voice)
                    loud_max_voice = voices[i].sent / 16;
            }
        loud_sum += loud;
        if (mix_gain < gain_min)
            gain_min = mix_gain;
        gain_low += mix_gain < 192;
        if (loud > loud_peak)
            loud_peak = loud;
        if (n > peak)
            peak = n;
        busy_frames += n == MAX_VOICES;
        sum += n;
        if (vol_msgs - msgs > peak_msgs)
            peak_msgs = vol_msgs - msgs;
    }
    printf("voices: peak %d, average %.1f, frames with all %d busy: %d of %d\n", peak, (double)sum / (secs * 60),
           MAX_VOICES, busy_frames, secs * 60);
    printf("notes started %d, channel kills %d, no free DS channel %d, volume/pan/freq messages %d (peak %d a frame)\n",
           plays, kills, fails, vol_msgs, peak_msgs);
    printf("steals: of a held note %d, of a fading one %d\n", music_steals_held, music_steals_fading);
    printf("loudness (sum of channel volumes): peak %d, average %.1f, loudest voice %d\n", loud_peak,
           (double)loud_sum / (secs * 60), loud_max_voice);
    printf("limiter: lowest gain %d/256, frames below 192/256: %d\n", gain_min, gain_low);
    return 0;
}
