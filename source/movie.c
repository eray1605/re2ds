// re2ds - movie player (see movie.h).
//
// Pictures: each frame is an LZ77 packed 8 bpp image with its own palette,
// unpacked in RAM and copied to the BG3 bitmap (VRAM E) in the vblank.
// Sound: one block of 16-bit mono PCM after the frames, streamed into a ring
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

#include "movie.h"

#define MOVIE_MAGIC 0x31564D52  // "RMV1"
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
    if (!fv || !fa || fread(&h, sizeof(h), 1, fv) != 1 || h.magic != MOVIE_MAGIC ||
        h.width != 256 || h.height > 128 || !h.fps) {
        if (fv) fclose(fv);
        if (fa) fclose(fa);
        return 1;
    }
    uint8_t *lz = malloc(h.max_lz);
    // the sound ring only while a movie plays (64 KB; RAM is short)
    int16_t *ring = memalign(32, RING * 2);
    if (!lz || !ring) {
        free(lz);
        free(ring);
        fclose(fv);
        fclose(fa);
        return 1;
    }
    fseek(fa, h.audio_offset, SEEK_SET);

    REG_DISPCNT &= ~DISPLAY_BG0_ACTIVE; // the 3D layer
    REG_DISPCNT &= ~DISPLAY_SPR_ACTIVE;
    clear_screen();
    setBrightness(1, 0);
    setBrightness(2, -16);

    // sound: fill the ring, then start it looping
    uint32_t written = 0; // samples queued in total
    memset(ring, 0, RING * 2);
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
            if (fread(ring + pos, 2, n, fa) != (size_t)n)
                n = 0;
            DC_FlushRange(ring + pos, n * 2);
            written += n;
            if (!n)
                break;
        }
        if (channel < 0) {
            channel = soundPlaySample(ring, SoundFormat_16Bit, RING * 2, h.rate, 127, 64, true, 0);
            clock_start();
        }

        // the frame due now (frames fall behind: skip, but keep reading)
        int due = (int)((uint64_t)t * h.fps / CLOCK_HZ);
        if (due >= h.frames)
            break;
        while (decoded < due) {
            uint32_t size;
            if (fread(&size, 4, 1, fv) != 1 || size > h.max_lz || fread(palette, 2, 256, fv) != 256 ||
                fread(lz, 1, size, fv) != size) {
                due = h.frames;
                break;
            }
            decoded++;
            if (decoded == due)
                decompress(lz, picture, LZ77);
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
    }
    clock_stop();
    if (channel >= 0)
        soundKill(channel);
    free(lz);
    free(ring);
    fclose(fv);
    fclose(fa);
    swiWaitForVBlank();
    clear_screen();
    return skipped;
}

void movie_end(void)
{
    clear_screen();
    REG_DISPCNT |= DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE;
    setBrightness(1, -16);
    setBrightness(2, 0);
}
