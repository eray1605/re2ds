// re2ds - movie player (see movie.h).
//
// Pictures: "RMV3" (tools/str2ds.py): groups of frames share a palette; the
// first of a group is a whole LZ77 packed 8 bpp image (a key frame), the
// others only the 8 x 8 tiles that changed (a mask of the tiles, then their
// pixels), so every frame is unpacked in order onto the last one. "RMV1" /
// "RMV2": each frame whole with its own palette. The picture is built in RAM
// and copied to the BG3 bitmap (VRAM E) in the vblank.
// Sound: one block of 8-bit ("RMV2"; "RMV1": 16-bit) mono PCM after the frames, streamed into a ring
// buffer the ARM7 loops over. The ARM7's position is not visible here, so
// both are paced by a 1 kHz tick task started with the sound (calico's tick
// counter itself stands still while no tick task runs):
// the picture shows the frame due by then, the ring is kept filled about a
// second ahead of where the sound must be. (Counting loop passes as vblanks
// ran slow whenever reading a frame took longer than a vblank - from the SD
// card it often does - and the sound then replayed old parts of the ring.)
#include <nds.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "menu.h"
#include "movie.h"
#include "subs.h"

#define MOVIE_MAGIC 0x31564D52  // "RMV1": 16-bit sound
#define MOVIE_MAGIC8 0x32564D52 // "RMV2": 8-bit sound (half the size)
#define MOVIE_MAGIC3 0x33564D52 // "RMV3": key frames and tile deltas, 8-bit sound
#define KEY_FRAME 0x80000000
#define TILES (256 / 8 * 128 / 8)
#define DELTA_MAX (TILES / 8 + 256 * 128) // a delta frame unpacked: the mask, at most every tile
#define RING 32768               // samples (64 KB, about 1.5 s)
#define LEAD 22050               // samples kept queued ahead of the sound
#define CLOCK_HZ 1000            // the movie clock counts milliseconds
#define LETTERBOX 32             // rows above the 256 x 128 picture

typedef struct {
    uint32_t magic;
    uint16_t width, height, fps, frames;
    uint32_t rate, audio_offset, audio_samples, max_lz;
} MovieHeader;

static uint8_t picture[256 * 128] __attribute__((aligned(32)));
static uint16_t palette[256];

static TickTask clock_task;
static volatile uint32_t clock_ms;

static void clock_tick(TickTask *t)
{
    (void)t;
    clock_ms++;
}

static void clock_start(void)
{
    clock_ms = 0;
    tickTaskStart(&clock_task, clock_tick, ticksFromHz(CLOCK_HZ), ticksFromHz(CLOCK_HZ));
}

static uint32_t clock_ticks(void) { return clock_ms; }

static void clock_stop(void) { tickTaskStop(&clock_task); }

static void clear_screen(void)
{
    dmaFillWords(0, BG_GFX, 256 * 256);
    BG_PALETTE[0] = 0;
}

int movie_play(const char *name)
{
    char path[48];
    snprintf(path, sizeof(path), "nitro:/movie/%s.bin", name);
    FILE *fv = fopen(path, "rb"), *fa = fopen(path, "rb");
    MovieHeader h;
    if (!fv || !fa || fread(&h, sizeof(h), 1, fv) != 1 ||
        (h.magic != MOVIE_MAGIC && h.magic != MOVIE_MAGIC8 && h.magic != MOVIE_MAGIC3) || h.width != 256 ||
        h.height > 128 || !h.fps) {
        if (fv) fclose(fv);
        if (fa) fclose(fa);
        return 1;
    }
    uint8_t *lz = malloc(h.max_lz);
    // the sound ring and the delta buffer only while a movie plays (RAM is short)
    int deltas = h.magic == MOVIE_MAGIC3;
    int bps = h.magic == MOVIE_MAGIC ? 2 : 1; // bytes per sample
    uint8_t *ring = memalign(32, RING * bps);
    uint8_t *delta = deltas ? memalign(32, DELTA_MAX) : NULL;
    if (!lz || !ring || (deltas && !delta)) {
        free(lz);
        free(ring);
        free(delta);
        fclose(fv);
        fclose(fa);
        return 1;
    }
    fseek(fa, h.audio_offset, SEEK_SET);

    REG_DISPCNT &= ~DISPLAY_BG0_ACTIVE; // the 3D layer
    REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE;
    clear_screen();
    setBrightness(1, 0);
    // the lower screen stays dark, or shows the subtitles if there are some
    int subs = subs_have(name);
    if (subs)
        menu_subtitle_blank(1);
    setBrightness(2, subs ? 0 : -16);

    // sound: fill the ring, then start it looping
    uint32_t written = 0; // samples queued in total
    memset(ring, 0, RING * bps);
    int channel = -1;
    int skipped = 0, shown = -1, decoded = -1;
    for (int first = 1;; first = 0) {
        // the time since the sound started; samples it has reached by now
        uint32_t t = first ? 0 : clock_ticks();
        uint32_t played = (uint32_t)((uint64_t)t * h.rate / CLOCK_HZ);
        while (written < h.audio_samples && written < played + LEAD) {
            int pos = written % RING;
            int n = RING - pos;
            if (n > 4096)
                n = 4096;
            if (written + n > h.audio_samples)
                n = h.audio_samples - written;
            if (fread(ring + pos * bps, bps, n, fa) != (size_t)n)
                n = 0;
            DC_FlushRange(ring + pos * bps, n * bps);
            written += n;
            if (!n)
                break;
        }
        if (channel < 0) {
            channel = soundPlaySample(ring, bps == 1 ? SoundFormat_8Bit : SoundFormat_16Bit, RING * bps, h.rate,
                                      127, 64, true, 0);
            clock_start();
        }

        // the frame due now (frames fall behind: skip, but keep reading)
        int due = (int)((uint64_t)t * h.fps / CLOCK_HZ);
        if (due >= h.frames)
            break;
        while (decoded < due) {
            uint32_t size;
            if (fread(&size, 4, 1, fv) != 1) {
                due = h.frames;
                break;
            }
            int key = !deltas || (size & KEY_FRAME);
            size &= ~KEY_FRAME;
            if (size > h.max_lz || (key && fread(palette, 2, 256, fv) != 256) || fread(lz, 1, size, fv) != size) {
                due = h.frames;
                break;
            }
            decoded++;
            if (key) {
                if (deltas || decoded == due) // RMV1 / 2: skipped frames need no unpacking
                    decompress(lz, picture, LZ77);
            } else { // the changed tiles onto the last picture
                decompress(lz, delta, LZ77);
                const uint8_t *mask = delta, *tile = delta + TILES / 8;
                for (int t = 0; t < TILES; t++)
                    if (mask[t >> 3] >> (t & 7) & 1) {
                        uint8_t *dst = picture + (t / 32) * 8 * 256 + (t % 32) * 8;
                        for (int y = 0; y < 8; y++, tile += 8)
                            memcpy(dst + y * 256, tile, 8);
                    }
            }
        }
        if (due >= h.frames)
            break;

        scanKeys();
        if (keysDown() & KEY_START) {
            skipped = 1;
            break;
        }
        swiWaitForVBlank();
        if (decoded != shown) {
            DC_FlushRange(picture, sizeof(picture));
            DC_FlushRange(palette, sizeof(palette));
            dmaCopy(picture, (uint8_t *)BG_GFX + LETTERBOX * 256, 256 * h.height);
            dmaCopy(palette + 1, BG_PALETTE + 1, 255 * 2);
            shown = decoded;
        }
        if (subs)
            menu_subtitle(subs_at(name, (uint32_t)((uint64_t)t * 1000 / CLOCK_HZ)));
    }
    clock_stop();
    if (channel >= 0)
        soundKill(channel);
    free(lz);
    free(ring);
    free(delta);
    fclose(fv);
    fclose(fa);
    swiWaitForVBlank();
    clear_screen();
    if (subs) {
        menu_subtitle_blank(0);
        setBrightness(2, -16); // as without subtitles: movie_end brings it back
    }
    return skipped;
}

void movie_end(void)
{
    clear_screen();
    REG_DISPCNT |= DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE;
    setBrightness(1, -16);
    setBrightness(2, 0);
}
