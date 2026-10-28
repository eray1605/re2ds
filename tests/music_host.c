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
static int ch_used[HOST_CHANNELS], plays, kills, steals_seen, fails, vol_msgs;
static int soundPlaySample(const void *d, int fmt, uint32_t size, uint32_t rate, int vol, int pan, bool loop, uint16_t lp)
{
    (void)d; (void)fmt; (void)size; (void)rate; (void)vol; (void)pan; (void)loop; (void)lp;
    for (int i = 0; i < HOST_CHANNELS; i++)
        if (!ch_used[i]) {
            ch_used[i] = 1;
            plays++;
            return i;
        }
    fails++;
    return -1;
}
static void soundKill(int ch) { if (ch >= 0 && ch < HOST_CHANNELS) ch_used[ch] = 0; kills++; }
static void soundSetVolume(int ch, int v) { (void)ch; (void)v; vol_msgs++; }
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
    int peak = 0, busy_frames = 0, peak_msgs = 0;
    long sum = 0;
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
        clock_frames += (argc > 5 && f % 2) ? 0 : (argc > 5 ? 2 : 1); // optional: a loop pass every 2 frames
        music_frame();
        int n = 0;
        for (int i = 0; i < MAX_VOICES; i++)
            n += voices[i].ch >= 0;
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
    return 0;
}
