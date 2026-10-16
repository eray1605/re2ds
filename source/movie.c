// re2ds - movie player (see movie.h).
//
// Pictures: each frame is an LZ77 packed 8 bpp image with its own palette,
// unpacked in RAM and copied to the BG3 bitmap (VRAM E) in the vblank.
// Sound: one block of 16-bit mono PCM after the frames, streamed into a ring
// buffer the ARM7 loops over. The ARM7's position is not visible here, so
// both are paced by counted vblanks (59.8261 Hz): the picture shows frame
// n at vblank n * 59.8261 / fps, the ring is kept filled ahead of where the
// sound must be.
#include <nds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "movie.h"

#define MOVIE_MAGIC 0x31564D52  // "RMV1"
#define RING 16384               // samples (32 KB, about 0.74 s)
#define LEAD 8192                // samples kept queued ahead of the sound
#define VBLANK_MILLIHZ 59826     // DS refresh: 59.8261 Hz
#define LETTERBOX 32             // rows above the 256 x 128 picture

typedef struct {
    uint32_t magic;
    uint16_t width, height, fps, frames;
    uint32_t rate, audio_offset, audio_samples, max_lz;
} MovieHeader;

static int16_t ring[RING] __attribute__((aligned(32)));
static uint8_t picture[256 * 128] __attribute__((aligned(32)));
static uint16_t palette[256];

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
    if (!lz) {
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
    memset(ring, 0, sizeof(ring));
    int channel = -1;
    int skipped = 0, shown = -1, decoded = -1;
    for (uint32_t vb = 0;; vb++) {
        // samples the sound has reached by now
        uint32_t played = (uint32_t)((uint64_t)vb * h.rate * 1000 / VBLANK_MILLIHZ);
        while (written < h.audio_samples && written < played + LEAD) {
            int pos = written % RING;
            int n = RING - pos;
            if (n > 1024)
                n = 1024;
            if (written + n > h.audio_samples)
                n = h.audio_samples - written;
            if (fread(ring + pos, 2, n, fa) != (size_t)n)
                n = 0;
            DC_FlushRange(ring + pos, n * 2);
            written += n;
            if (!n)
                break;
        }
        if (channel < 0)
            channel = soundPlaySample(ring, SoundFormat_16Bit, sizeof(ring), h.rate, 127, 64, true, 0);

        // the frame due now (frames fall behind: skip, but keep reading)
        int due = (int)((uint64_t)vb * h.fps * 1000 / VBLANK_MILLIHZ);
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
    if (channel >= 0)
        soundKill(channel);
    free(lz);
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
