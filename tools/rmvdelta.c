// Tile deltas for the DS movies (tools/str2ds.py, source/movie.c).
//
// stdin: frames of one group that share a palette:
//   u32 count, u8 palette[256 * 3] (r, g, b), then count x u8 index[W * H]
// stdout: per frame u32 size + packet:
//   the first frame: the W * H indices as they are (a key frame);
//   the others: u8 mask[tiles / 8] (bit set = the 8 x 8 tile changed), then
//   the changed tiles' 64 indices each, in tile order.
// A tile is sent again when its colours differ from what is on screen by
// more than the thresholds (mean and largest difference per channel). Small
// changes - the noise of the PS1's video compression, the dithering - are
// left alone, so most of a still picture is not sent again. Two more rules
// keep the left-alone tiles from showing as blocks:
// - a tile next to one that is sent goes along when it differs by more than
//   half the thresholds (no edge between a fresh tile and a stale one);
// - a tile left alone for STALE frames goes when it differs at all visibly.
// stderr (with -v): how many tiles stay visibly wrong (mean difference over
// VISIBLE) - the measure for choosing the thresholds.
//
// Usage: rmvdelta W H [mean max [-v]]   (defaults 4 32)
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STALE 8
#define VISIBLE 3

int main(int argc, char **argv)
{
    if (argc < 3)
        return 1;
    int w = atoi(argv[1]), h = atoi(argv[2]);
    int mean_max = argc > 3 ? atoi(argv[3]) : 4, diff_max = argc > 4 ? atoi(argv[4]) : 32;
    int verbose = argc > 5 && !strcmp(argv[5], "-v");
    int tw = w / 8, th = h / 8, tiles = tw * th;
    uint32_t count;
    uint8_t pal[256 * 3];
    if (fread(&count, 4, 1, stdin) != 1 || fread(pal, 1, sizeof(pal), stdin) != sizeof(pal))
        return 1;
    uint8_t *cur = malloc(w * h), *shown = malloc(w * h), *packet = malloc(tiles / 8 + w * h);
    int *sum = malloc(tiles * sizeof(int)), *big = malloc(tiles * sizeof(int));
    uint8_t *send = malloc(tiles), *age = calloc(tiles, 1);
    long wrong = 0, sent = 0;
    for (uint32_t f = 0; f < count; f++) {
        if (fread(cur, 1, w * h, stdin) != (size_t)(w * h))
            return 1;
        uint32_t size;
        if (f == 0) {
            memcpy(shown, cur, w * h);
            size = w * h;
            fwrite(&size, 4, 1, stdout);
            fwrite(cur, 1, size, stdout);
            continue;
        }
        // how far each tile on screen is from the new picture
        for (int t = 0; t < tiles; t++) {
            int x0 = (t % tw) * 8, y0 = (t / tw) * 8, s = 0, b = 0;
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++) {
                    int i = (y0 + y) * w + x0 + x;
                    const uint8_t *p = pal + 3 * cur[i], *q = pal + 3 * shown[i];
                    for (int c = 0; c < 3; c++) {
                        int d = abs(p[c] - q[c]);
                        s += d;
                        if (d > b)
                            b = d;
                    }
                }
            sum[t] = s / (64 * 3); // mean difference per channel
            big[t] = b;
            send[t] = sum[t] > mean_max || b > diff_max || (age[t] >= STALE && sum[t] >= VISIBLE);
        }
        // the neighbours of a sent tile go along from half the thresholds on
        for (int t = 0; t < tiles; t++) {
            if (send[t] || (sum[t] * 2 <= mean_max && big[t] * 2 <= diff_max))
                continue;
            int x = t % tw, y = t / tw;
            if ((x > 0 && send[t - 1] == 1) || (x < tw - 1 && send[t + 1] == 1) || (y > 0 && send[t - tw] == 1) ||
                (y < th - 1 && send[t + tw] == 1))
                send[t] = 2;
        }
        uint8_t *mask = packet, *out = packet + tiles / 8;
        memset(mask, 0, tiles / 8);
        for (int t = 0; t < tiles; t++) {
            if (!send[t]) {
                if (age[t] < 255)
                    age[t]++;
                if (sum[t] >= VISIBLE)
                    wrong++;
                continue;
            }
            int x0 = (t % tw) * 8, y0 = (t / tw) * 8;
            age[t] = 0;
            sent++;
            mask[t >> 3] |= 1 << (t & 7);
            for (int y = 0; y < 8; y++) {
                memcpy(out, cur + (y0 + y) * w + x0, 8);
                memcpy(shown + (y0 + y) * w + x0, cur + (y0 + y) * w + x0, 8);
                out += 8;
            }
        }
        size = (uint32_t)(out - packet);
        fwrite(&size, 4, 1, stdout);
        fwrite(packet, 1, size, stdout);
    }
    if (verbose)
        fprintf(stderr, "%ld %ld\n", wrong, sent);
    return 0;
}
