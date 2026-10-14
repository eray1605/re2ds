// LZ77 compressor for the DS BIOS (type 0x10, "LZ77UnCompVram" safe).
//
// Format: u32 header = 0x10 | (uncompressed size << 8), then blocks of one
// flag byte (MSB first, 1 = match) and 8 items. A literal is one byte; a match
// is two bytes: (length - 3) << 4 | (distance - 1) >> 8, (distance - 1) & 0xFF
// with length 3..18 and distance 1..4096. Distance 1 is never used: the VRAM
// variant writes 16 bits at a time and cannot copy from the byte it is
// still assembling.
//
// Usage: lz77 in out
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MIN_LEN 3
#define MAX_LEN 18
#define WINDOW 4096
#define MIN_DIST 2

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s in out\n", argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *in = malloc(n ? n : 1);
    if (fread(in, 1, n, f) != (size_t)n) {
        perror(argv[1]);
        return 1;
    }
    fclose(f);

    // worst case: 9 bytes per 8 literals
    uint8_t *out = malloc(4 + n + n / 8 + 16);
    long o = 0;
    out[o++] = 0x10;
    out[o++] = n & 0xFF;
    out[o++] = (n >> 8) & 0xFF;
    out[o++] = (n >> 16) & 0xFF;

    // hash chains over 3-byte prefixes
    int32_t *head = malloc(sizeof(int32_t) * 65536);
    int32_t *prev = malloc(sizeof(int32_t) * (n ? n : 1));
    for (int i = 0; i < 65536; i++)
        head[i] = -1;
#define HASH(p) ((uint32_t)((in[p] << 8) ^ (in[(p) + 1] << 4) ^ in[(p) + 2]) & 0xFFFF)

    long pos = 0, inserted = 0;
    while (pos < n) {
        long flag_at = o++;
        uint8_t flags = 0;
        for (int item = 0; item < 8 && pos < n; item++) {
            // index every position before pos
            for (; inserted < pos && inserted + 2 < n; inserted++) {
                uint32_t h = HASH(inserted);
                prev[inserted] = head[h];
                head[h] = (int32_t)inserted;
            }
            int best_len = 0, best_dist = 0;
            if (pos + MIN_LEN <= n) {
                int chain = 0;
                for (int32_t c = head[HASH(pos)]; c >= 0 && pos - c <= WINDOW && chain < 512;
                     c = prev[c], chain++) {
                    int dist = (int)(pos - c);
                    if (dist < MIN_DIST)
                        continue;
                    int len = 0;
                    while (len < MAX_LEN && pos + len < n && in[c + len] == in[pos + len])
                        len++;
                    if (len > best_len) {
                        best_len = len;
                        best_dist = dist;
                        if (len == MAX_LEN)
                            break;
                    }
                }
            }
            if (best_len >= MIN_LEN) {
                flags |= 0x80 >> item;
                out[o++] = ((best_len - 3) << 4) | ((best_dist - 1) >> 8);
                out[o++] = (best_dist - 1) & 0xFF;
                pos += best_len;
            } else {
                out[o++] = in[pos++];
            }
        }
        out[flag_at] = flags;
    }
    while (o & 3)
        out[o++] = 0;

    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, o, f) != (size_t)o) {
        perror(argv[2]);
        return 1;
    }
    fclose(f);
    return 0;
}
