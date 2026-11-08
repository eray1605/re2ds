// re2ds - the ending (COMMON/BIN/ENDING.BIN, sub-program 5 of the table
// 0x8009a4d4, loaded at 0x8011a000). The last room's script fades out and
// sets system flag 0:0x0a; the PS1's main loop (0x80025ed4) then blanks the
// screen and runs this program. Its step table (0x8011a024, 19 steps, a frame
// counter at 0x8011f0b0) for scenario A: the ending music MAIN26 for 300
// frames, then movie 17 (R704L); B plays movie 14 at once. Then the picture
// END10 (Leon) / END11 (Claire) with MAIN2A; after 480 frames (START: at
// once) the results fade in over it - CONGRATULATIONS!!, RANKING and the
// letter, CLEAR GAME and the scenario, TOTAL TIME, NUMBER OF SAVES - for 1200
// more frames (START ends them), and the picture fades out. The PS1 then
// offers the rewards (special weapons for the next game), saves the clear
// data for the other scenario (MEM_CARD.BIN) and its extra modes, and goes
// back to the title; the DS has none of that yet. Pictures and texts:
// tools/ending2ds.py. Frames here are game ticks (30 Hz).
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "ending.h"
#include "formats.h"
#include "movie.h"
#include "music.h"

#define TEXT_MAGIC 0x444E4552 // "REND"
#define TEXT_FIRST 224        // the texts' palette entries
#define TEXT_SPRITES 31       // the table's texts; the glyphs 0-9 and ':' follow
#define GLYPH_COLON 10
#define FADE_TICKS 16

typedef struct {
    int16_t x, y;
    uint16_t w, h;
    const uint8_t *px;
} Sprite;

static Sprite sprites[TEXT_SPRITES + 11];
static uint16_t text_palette[32];
static uint16_t num_x[10], num_y[10];
static uint16_t pic_palette[256];

// one game tick: two DS frames, the music going on
static void tick(void)
{
    for (int i = 0; i < 2; i++) {
        swiWaitForVBlank();
        music_frame();
    }
    scanKeys();
}

static int load_text(void *data)
{
    const uint8_t *p = data;
    if (!p || *(const uint32_t *)p != TEXT_MAGIC)
        return 0;
    memcpy(text_palette, p + 4, sizeof(text_palette));
    int n = p[68] | p[69] << 8;
    p += 70;
    for (int i = 0; i < n; i++) {
        Sprite s;
        memcpy(&s, p, 8);
        s.px = p + 8;
        p += 8 + s.w * s.h;
        if (i < TEXT_SPRITES + 11)
            sprites[i] = s;
    }
    memcpy(num_x, p, sizeof(num_x));
    memcpy(num_y, p + 20, sizeof(num_y));
    return 1;
}

static int load_picture(int n, uint8_t *work)
{
    char path[32];
    snprintf(path, sizeof(path), "nitro:/ending/pic%d.bin", n);
    BgHeader *bg = asset_load(path, NULL);
    if (!bg || bg->magic != BG_MAGIC) {
        asset_free(bg);
        memset(work, 0, BG_WIDTH * BG_HEIGHT);
        memset(pic_palette, 0, sizeof(pic_palette));
        return 0;
    }
    decompress(bg + 1, work, LZ77);
    memcpy(pic_palette, bg->palette, sizeof(pic_palette));
    asset_free(bg);
    return 1;
}

static void blit(uint8_t *work, const Sprite *s, int x, int y)
{
    for (int j = 0; j < s->h; j++)
        for (int i = 0; i < s->w; i++) {
            uint8_t v = s->px[j * s->w + i];
            if (v && x + i >= 0 && x + i < BG_WIDTH && y + j >= 0 && y + j < BG_HEIGHT)
                work[(y + j) * BG_WIDTH + x + i] = v;
        }
}

static void show(const uint8_t *work)
{
    DC_FlushRange(work, BG_WIDTH * BG_HEIGHT);
    swiWaitForVBlank();
    dmaCopy(work, BG_GFX, BG_WIDTH * BG_HEIGHT);
}

// the picture's colours, and the texts' at level 0..16
static void set_palette(int text_level)
{
    for (int i = 1; i < TEXT_FIRST; i++)
        BG_PALETTE[i] = pic_palette[i];
    for (int i = 0; i < 32; i++) {
        uint16_t c = text_palette[i];
        int r = (c & 31) * text_level / 16, g = (c >> 5 & 31) * text_level / 16, b = (c >> 10 & 31) * text_level / 16;
        BG_PALETTE[TEXT_FIRST + i] = r | g << 5 | b << 10;
    }
}

static void fade_picture(int in)
{
    for (int k = 0; k <= FADE_TICKS; k++) {
        setBrightness(1, in ? -16 + k : -k);
        tick();
    }
}

// ENDING.BIN 0x8011b9dc: the clear time in hours over 2.5 (at most 10)
// costs 10 points each, every save 5 (20 or more: all 100), every first aid
// spray 15 (6 or more: all 100); a special weapon takes 50 off the sum. The
// rank is how many of 269, 199, 144, 99, 0 the sum stays under: A .. E.
int ending_rank(const EndingStats *st, int *time_points, int *save_points, int *spray_points)
{
    int over = 0;
    for (uint32_t limit = 540000; st->frames > limit && over < 10; limit += 216000)
        over++;
    int t = 100 - over * 10, s = st->saves >= 20 ? 0 : 100 - (int)st->saves * 5;
    int f = st->sprays >= 6 ? 0 : 100 - (int)st->sprays * 15;
    int sum = t + s + f;
    if (st->special_weapon)
        sum = sum >= 50 ? sum - 50 : 0;
    static const int limits[5] = { 269, 199, 144, 99, 0 };
    int rank = 0;
    while (rank < 5 && sum < limits[rank])
        rank++;
    if (time_points) *time_points = t;
    if (save_points) *save_points = s;
    if (spray_points) *spray_points = f;
    return rank;
}

static void results(const EndingStats *st, uint8_t *work)
{
    int rank = ending_rank(st, NULL, NULL, NULL);
    // the texts: the labels 13-17, the letter (4 + rank), the scenario (9-12)
    for (int k = 13; k <= 17; k++)
        blit(work, &sprites[k], sprites[k].x, sprites[k].y);
    const Sprite *letter = &sprites[4 + rank], *name = &sprites[9 + st->claire + 2 * st->scenario_b];
    blit(work, letter, letter->x, letter->y);
    blit(work, name, name->x, name->y);
    // hh:mm:ss and the saves (0x8011ba20): two digits each, "99" beyond
    uint32_t h = st->frames / 216000, m = st->frames / 3600 % 60, s = st->frames / 60 % 60;
    unsigned saves = st->saves;
    int digits[10] = { h >= 99 ? 9 : (int)h / 10, h >= 99 ? 9 : (int)h % 10, GLYPH_COLON, (int)m / 10, (int)m % 10,
                       GLYPH_COLON, (int)s / 10, (int)s % 10, saves >= 99 ? 9 : (int)saves / 10,
                       saves >= 99 ? 9 : (int)saves % 10 };
    for (int i = 0; i < 10; i++)
        blit(work, &sprites[TEXT_SPRITES + digits[i]], num_x[i], num_y[i]);
}

void ending_run(const EndingStats *st, uint8_t *work)
{
    void *text = asset_load("nitro:/ending/text.bin", NULL);
    int have_text = load_text(text);

    // the screen black, the game's layers off (as movie_play leaves it)
    setBrightness(1, -16);
    setBrightness(2, -16);
    REG_DISPCNT &= ~(DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE);
    memset(work, 0, BG_WIDTH * BG_HEIGHT);
    show(work);
    for (int i = 0; i < 3; i++)
        music_control(i, 2, 0, 0, 0);

    // steps 0 / 1: the movie (scenario A: the ending music first, 300 frames)
    if (!st->scenario_b) {
        music_pair(0x26, 0xFF);
        for (int t = 0; t < 300; t++)
            tick();
    }
    movie_play("r704l");
    movie_end();
    REG_DISPCNT &= ~(DISPLAY_BG0_ACTIVE | DISPLAY_SPR_ACTIVE);
    setBrightness(1, -16);
    setBrightness(2, -16);
    for (int i = 0; i < 3; i++)
        music_control(i, 2, 0, 0, 0);

    // steps 4 / 5: the picture, the music MAIN2A, then the results
    load_picture(st->claire ? 1 : 0, work);
    set_palette(0);
    show(work);
    music_pair(0x2A, 0xFF);
    fade_picture(1);
    int shown = 0;
    for (int t = 0;; t++) {
        tick();
        int start = (keysDown() & (KEY_START | KEY_A)) != 0;
        if (!shown && (t >= 480 || start) && have_text) {
            results(st, work);
            show(work);
            for (int k = 0; k <= FADE_TICKS; k++) {
                set_palette(k);
                tick();
            }
            shown = 1;
            t = 0;
            continue;
        }
        if ((shown && (t >= 1200 || (start && t >= 300))) || (!have_text && t >= 480))
            break;
    }
    for (int i = 0; i < 3; i++)
        music_control(i, 5, 0, 0, 0); // fades out (FUN_80056c20)
    fade_picture(0);
    for (int t = 0; t < 60; t++) // 0x8011a334: 60 frames black, then the title
        tick();
    for (int i = 0; i < 3; i++)
        music_control(i, 2, 0, 0, 0);
    asset_free(text);
    memset(work, 0, BG_WIDTH * BG_HEIGHT);
    show(work);
}
