// re2ds - the messages on the upper screen (see msgtop.h). Eight 64 x 32
// sprites (16 colours, VRAM F, OAM 2-9) cover the bottom 256 x 64 of the
// picture; the text is drawn into a 4 bpp buffer and copied as tiles.
#include <nds.h>
#include <string.h>

#include "assets.h"
#include "msgtop.h"

#define BUF_W 256
#define BUF_H 64
#define FIRST_OAM 2
#define SPRITES 8
#define PALETTE 1                // sprite palette slot
#define LEFT 8
#define WRAP 248
#define LINE_GAP 2
#define HIGHLIGHT 7              // + shade: the green colours
#define SHADOW 15

static const uint8_t *font;
static int font_h, font_n;
static const uint8_t *glyph_at[128];
static uint8_t glyph_w[128];
static u16 *gfx[SPRITES];
static uint8_t buf[BUF_H][BUF_W];     // one colour index a pixel
static int shown;

void msgtop_init(void)
{
    font = asset_load("nitro:/menu/font.bin", NULL);
    if (!font || memcmp(font, "RFN1", 4)) {
        asset_free((void *)font);
        font = NULL;
        return;
    }
    font_h = font[4];
    font_n = font[5];
    const uint8_t *p = font + 6;
    for (int i = 0; i < font_n; i++) {
        int c = p[0], w = p[1];
        if (c < 128) {
            glyph_at[c] = p + 2;
            glyph_w[c] = w;
        }
        p += 2 + w * font_h;
    }
    for (int s = 0; s < SPRITES; s++)
        gfx[s] = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_16Color);
    // shades 1-7 dark to white, 8-14 the same in green, 15 the shadow
    for (int k = 1; k <= 7; k++) {
        int v = 2 + (k - 1) * 29 / 6;
        SPRITE_PALETTE[PALETTE * 16 + k] = RGB15(v, v, v);
        SPRITE_PALETTE[PALETTE * 16 + HIGHLIGHT + k] = RGB15(v / 3, v, v / 3);
    }
    SPRITE_PALETTE[PALETTE * 16 + SHADOW] = RGB15(0, 0, 0);
}

static int char_width(int c)
{
    return c > 0 && c < 128 && glyph_at[c] ? glyph_w[c] + 1 : 0;
}

static void put(int c, int x, int y, int hl)
{
    if (c <= 0 || c >= 128 || !glyph_at[c])
        return;
    const uint8_t *g = glyph_at[c];
    int w = glyph_w[c];
    for (int pass = 0; pass < 2; pass++) // the shadow first, one pixel down and right
        for (int j = 0; j < font_h; j++)
            for (int i = 0; i < w; i++) {
                int v = g[j * w + i];
                int px = x + i + (pass ? 0 : 1), py = y + j + (pass ? 0 : 1);
                if (!v || px < 0 || px >= BUF_W || py < 0 || py >= BUF_H)
                    continue;
                buf[py][px] = pass ? (hl ? HIGHLIGHT + v : v) : SHADOW;
            }
}

// the width of the word starting at s (to a space, a new line or the end)
static int word_width(const char *s, const char *end)
{
    int w = 0;
    for (; s < end && *s != ' ' && *s != '\n'; s++)
        w += char_width((unsigned char)*s);
    return w;
}

static void upload(void)
{
    // 64 x 32 sprites, 1D: 8 x 4 tiles of 8 x 8, 32 bytes each
    static u32 tiles[64 * 32 / 8];
    for (int s = 0; s < SPRITES; s++) {
        int sx = (s % 4) * 64, sy = (s / 4) * 32;
        u32 *t = tiles;
        for (int ty = 0; ty < 4; ty++)
            for (int tx = 0; tx < 8; tx++)
                for (int y = 0; y < 8; y++) {
                    const uint8_t *row = &buf[sy + ty * 8 + y][sx + tx * 8];
                    u32 v = 0;
                    for (int x = 0; x < 8; x++)
                        v |= (u32)(row[x] & 15) << (4 * x);
                    *t++ = v;
                }
        DC_FlushRange(tiles, sizeof(tiles));
        dmaCopy(tiles, gfx[s], sizeof(tiles));
    }
}

// the width of a laid-out line, without the trailing spaces
static int line_width(const char *s, const char *e)
{
    while (e > s && e[-1] == ' ')
        e--;
    int w = 0;
    for (; s < e; s++) {
        int c = (unsigned char)*s;
        if (c >= 0x20)
            w += char_width(c) ? char_width(c) : 4;
    }
    return w;
}

#define MAX_LINES 8

void msgtop_show(const char *s, const char *end, int choice, int answer)
{
    if (!font || !s)
        return;
    memset(buf, 0, sizeof(buf));
    // lay out the lines first (new lines and word wrap), then draw each one
    // centred, as the PS1 centres its message lines
    const char *ls[MAX_LINES], *le[MAX_LINES];
    int n = 0, w = 0;
    ls[0] = s;
    for (const char *p = s; p < end && *p && n < MAX_LINES; p++) {
        int c = (unsigned char)*p;
        if (c == '\n') {
            le[n++] = p;
            if (n < MAX_LINES)
                ls[n] = p + 1;
            w = 0;
            continue;
        }
        if (c < 0x20)
            continue;
        if (c != ' ' && (p == s || p[-1] == ' ') && w > 0 && w + word_width(p, end) > WRAP - LEFT) {
            le[n++] = p;
            if (n >= MAX_LINES)
                break;
            ls[n] = p;
            w = 0;
        }
        w += char_width(c) ? char_width(c) : 4;
    }
    if (n < MAX_LINES) {
        const char *e = ls[n];
        while (e < end && *e)
            e++;
        le[n++] = e;
    }
    int line_h = font_h + LINE_GAP, room = BUF_H / line_h;
    if (n + choice > room)
        n = room - choice;
    int y = BUF_H - (n + choice) * line_h, hl = 0;
    for (int k = 0; k < n; k++, y += line_h) {
        int x = (BUF_W - line_width(ls[k], le[k])) / 2;
        for (const char *p = ls[k]; p < le[k]; p++) {
            int c = (unsigned char)*p;
            if (c == 0x01) {
                hl = !hl;
                continue;
            }
            if (c < 0x20)
                continue;
            put(c, x, y, hl);
            x += char_width(c) ? char_width(c) : 4;
        }
    }
    if (choice) {
        int yy = BUF_H - line_h;
        static const char *const words[2] = { "YES", "NO" };
        for (int k = 0; k < 2; k++) {
            int xx = k ? 168 : 64;
            for (const char *w = words[k]; *w; w++) {
                put((unsigned char)*w, xx, yy, answer == k);
                xx += char_width((unsigned char)*w);
            }
        }
    }
    upload();
    for (int k = 0; k < SPRITES; k++)
        oamSet(&oamMain, FIRST_OAM + k, (k % 4) * 64, 192 - BUF_H + (k / 4) * 32, 0, PALETTE, SpriteSize_64x32,
               SpriteColorFormat_16Color, gfx[k], -1, false, false, false, false, false);
    oamUpdate(&oamMain);
    shown = 1;
}

void msgtop_hide(void)
{
    if (!shown)
        return;
    for (int k = 0; k < SPRITES; k++)
        oamSetHidden(&oamMain, FIRST_OAM + k, true);
    oamUpdate(&oamMain);
    shown = 0;
}
