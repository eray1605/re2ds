// re2ds - status screen (see menu.h).
//
// The lower screen is an 8 bpp bitmap (BG3 of the sub engine in VRAM H + I,
// exactly 256 x 192) in the palette of the PS1 item icons; a RAM copy is
// drawn in and sent over when it changed. Text uses libnds' 8 x 8 console font.
#include <nds.h>
#include <stdio.h>
#include <string.h>

#include "assets.h"
#include "menu.h"
#include "script.h"

#define ITEMS_MAGIC 0x31544952 // "RIT1"
#define ICON_W 40
#define ICON_H 30
#define PICTURES_MAGIC 0x31504952 // "RIP1"

enum { C_BLACK, C_DARK, C_GREY, C_LIGHT, C_WHITE, C_GREEN, C_YELLOW, C_RED, C_DIM_GREEN, C_DIM_YELLOW, C_DIM_RED };
enum { CMD_EQUIP, CMD_USE, CMD_CHECK, CMD_COMBINE, NUM_CMDS };
static const char *const cmd_names[NUM_CMDS] = { "EQUIP", "USE", "CHECK", "COMBINE" };

extern const u8 default_font_bin[];

static uint8_t fb[256 * 192];
static int dirty, bg = -1, debug_mode;
static uint32_t last_inv;
static uint8_t *data;
static const uint8_t *ui;              // ui colour indices
static const uint8_t *icons;
static const uint16_t *name_offs;
static const uint8_t *portrait;
static int portrait_w, portrait_h;
static u16 *pause_gfx[2];
static int pause_shown = -1;
static const char *names;
static int num_icons, num_names;
static int focus, sel, row;            // row 0 = slots, 1 = commands
static int cmd_sel;
static const char *message;
static char info[96];                  // text box when no message is open
static int ecg_phase;
static int combine_from = -1;          // COMBINE picked on this slot: choosing the other
static uint8_t *pictures;              // menu/pictures.bin (tools/itps2ds.py)
static int num_pictures, picture_w, picture_h;
static int picture;                    // item whose picture covers the slots (0 = none)

// Menu sounds from CORE00 (PS1 bank 4; the status screen plays 0x404..0x40A):
// by length 4 is the short cursor blip, 5 / 6 select and back, 7 the "no"
// blip, 8 the long reload click (assignment by ear, not traced to the calls).
enum { SND_CURSOR = 4, SND_SELECT = 5, SND_BACK = 6, SND_NO = 7, SND_RELOAD = 8 };
void game_menu_sound(int sound);

static int col(int c) { return ui ? ui[c] : (c == C_WHITE ? 255 : 0); }

static void fill(int x, int y, int w, int h, int c)
{
    for (int j = y; j < y + h && j < 192; j++)
        if (j >= 0)
            memset(fb + j * 256 + x, col(c), w);
    dirty = 1;
}

static void frame(int x, int y, int w, int h, int c)
{
    fill(x, y, w, 1, c);
    fill(x, y + h - 1, w, 1, c);
    fill(x, y, 1, h, c);
    fill(x + w - 1, y, 1, h, c);
}

static void text(int x, int y, const char *s, int c)
{
    for (; *s && x <= 248; s++, x += 8) {
        const u8 *g = default_font_bin + (uint8_t)*s * 8;
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                if (g[j] >> i & 1)
                    fb[(y + j) * 256 + x + i] = col(c);
    }
    dirty = 1;
}

// word-wrapped text from (x, y), lines of up to 30 characters, until y_end
static void text_wrapped(int x, int y, int y_end, const char *s, int c)
{
    char line[31];
    while (*s && y + 8 <= y_end) {
        int len = 0, last_space = -1;
        while (s[len] && s[len] != '\n' && len < 30) {
            if (s[len] == ' ')
                last_space = len;
            len++;
        }
        if (s[len] && s[len] != '\n' && last_space > 0)
            len = last_space;
        memcpy(line, s, len);
        line[len] = 0;
        text(x, y, line, c);
        s += len;
        while (*s == ' ' || *s == '\n')
            s++;
        y += 9;
    }
}

// word-wrapped text in the box at the bottom (5 lines of 30 characters)
static void text_box(const char *s, int c)
{
    fill(4, 150, 248, 40, C_BLACK);
    frame(4, 150, 248, 40, C_GREY);
    text_wrapped(8, 153, 188, s, c);
}

static void icon(int x, int y, int item)
{
    int n = item; // icon 0 is the empty slot
    if (!icons || n < 1 || n >= num_icons)
        return;
    const uint8_t *p = icons + n * ICON_W * ICON_H;
    for (int j = 0; j < ICON_H; j++)
        memcpy(fb + (y + j) * 256 + x, p + j * ICON_W, ICON_W);
    dirty = 1;
}

const char *item_name(int item)
{
    if (!names || item < 1 || item > num_names)
        return "";
    return names + name_offs[item - 1];
}

static int is_weapon(int item) { return item >= 0x01 && item <= 0x13; }

static void slot_pos(int i, int *x, int *y)
{
    *x = 8 + (i % 4) * 62;
    *y = 56 + (i / 4) * 40;
}

static void draw_slot(int i)
{
    int x, y;
    slot_pos(i, &x, &y);
    int c = focus && row == 0 && sel == i ? C_YELLOW : i == combine_from ? C_RED
          : i == inventory_equipped ? C_GREEN : C_DARK;
    fill(x, y, 48, 36, C_BLACK);
    frame(x, y, 48, 36, c);
    if (inventory[i].item) {
        icon(x + 4, y + 3, inventory[i].item);
        int item = inventory[i].item;
        if (item >= 0x02 && item <= 0x1E) { // guns, ammunition, ink ribbons (not the knife, herbs)
            char n[8];
            snprintf(n, sizeof(n), "%d", inventory[i].count);
            text(x + 46 - 8 * (int)strlen(n), y + 27, n, C_WHITE);
        }
    }
}

static void draw_commands(void)
{
    for (int k = 0; k < NUM_CMDS; k++) {
        int x = 6 + k * 62, c = focus && row == 1 && cmd_sel == k ? C_YELLOW : C_GREY;
        fill(x, 134, 58, 12, C_BLACK);
        frame(x, 134, 58, 12, c);
        text(x + (58 - 8 * (int)strlen(cmd_names[k])) / 2, 136, cmd_names[k], c == C_YELLOW ? C_YELLOW : C_LIGHT);
    }
}

static int last_hp = -9999;

// One heartbeat of the ECG, height (up = positive) at t of 0..period-1:
// baseline, P wave, QRS spike, T wave.
static int ecg_wave(int t)
{
    if (t >= 6 && t < 12)
        return (t < 9 ? t - 6 : 12 - t);          // P: small bump
    if (t == 15)
        return -2;                                 // Q
    if (t >= 16 && t < 19)
        return 4 + (t - 16) * 5;                   // R: steep up
    if (t >= 19 && t < 22)
        return 14 - (t - 19) * 8;                  // down through the line
    if (t == 22)
        return -6;                                 // S
    if (t == 23)
        return -2;
    if (t >= 28 && t < 38)
        return t < 33 ? (t - 28) / 2 + 1 : (38 - t) / 2; // T: wider bump
    return 0;
}

#define ECG_X 54
#define ECG_W 140
#define ECG_BASE 40

static void draw_condition(int hp)
{
    int c = hp < 21 ? C_RED : hp < 101 ? C_YELLOW : C_GREEN;
    int dim = hp < 21 ? C_DIM_RED : hp < 101 ? C_DIM_YELLOW : C_DIM_GREEN;
    const char *s = hp < 21 ? "DANGER " : hp < 101 ? "CAUTION" : "FINE   ";
    fill(52, 2, 144, 52, C_BLACK);
    text(54, 5, "CONDITION", C_GREY);
    text(54, 17, s, c);
    // a monitor sweep: the trace is written left to right at the cursor,
    // bright just behind it, dim further back, a gap just ahead of it
    int period = hp < 21 ? 40 : hp < 101 ? 48 : 60;
    int cursor = ecg_phase % ECG_W;
    int prev_y = -1;
    for (int i = 0; i < ECG_W; i++) {
        int ahead = (i - cursor + ECG_W) % ECG_W;
        if (ahead > 0 && ahead < 10) {
            prev_y = -1;
            continue;
        }
        int y = ECG_BASE - ecg_wave(i % period);
        int behind = (cursor - i + ECG_W) % ECG_W;
        int cc = behind < 50 ? c : dim;
        int y0 = prev_y < 0 ? y : prev_y;
        for (int k = y0 < y ? y0 : y; k <= (y0 > y ? y0 : y); k++)
            fb[k * 256 + ECG_X + i] = col(cc);
        prev_y = y;
    }
    dirty = 1;
}

static void draw_portrait(void)
{
    frame(2, 2, portrait_w + 4, portrait_h + 4, C_GREY);
    if (!portrait)
        return;
    for (int j = 0; j < portrait_h; j++)
        memcpy(fb + (4 + j) * 256 + 4, portrait + j * portrait_w, portrait_w);
}

static void draw_weapon(void)
{
    fill(200, 2, 52, 50, C_BLACK);
    frame(200, 2, 52, 40, C_GREY);
    if (inventory_equipped >= 0 && inventory[inventory_equipped].item) {
        int item = inventory[inventory_equipped].item;
        icon(206, 7, item);
        if (item != 0x01) {
            char n[16];
            snprintf(n, sizeof(n), "%d", inventory[inventory_equipped].count);
            text(250 - 8 * (int)strlen(n), 44, n, C_GREEN);
        }
    } else {
        text(206, 18, "NONE", C_GREY);
    }
}

// the PS1's item picture (ITPS.ITP), over the slots
static void draw_picture(void)
{
    if (!pictures || picture < 1 || picture >= num_pictures)
        return;
    int x = (256 - picture_w) / 2, y = 58;
    fill(x - 4, y - 4, picture_w + 8, picture_h + 8, C_BLACK);
    frame(x - 2, y - 2, picture_w + 4, picture_h + 4, C_GREY);
    const uint8_t *p = pictures + 12 + picture * picture_w * picture_h;
    for (int j = 0; j < picture_h; j++)
        memcpy(fb + (y + j) * 256 + x, p + j * picture_w, picture_w);
}

static void redraw(int hp)
{
    memset(fb, col(C_BLACK), sizeof(fb));
    draw_portrait();
    draw_condition(hp);
    draw_weapon();
    for (int i = 0; i < INV_SLOTS; i++)
        draw_slot(i);
    draw_commands();
    draw_picture();
    if (message)
        text_box(message, C_WHITE);
    else
        text_box(info, C_LIGHT);
    dirty = 1;
}

static void show_screen(void)
{
    videoSetModeSub(MODE_5_2D);
    vramSetBankH(VRAM_H_SUB_BG);
    vramSetBankI(VRAM_I_SUB_BG_0x06208000);
    bg = bgInitSub(3, BgType_Bmp8, BgSize_B8_256x256, 0, 0);
    if (data)
        memcpy(BG_PALETTE_SUB, data + 20, 512);
    last_hp = -9999;
}

// "PAUSE" on the upper screen: two 64 x 32 sprites (VRAM F), 2x font with a
// black outline
static void pause_init(void)
{
    static uint8_t buf[32][128];
    vramSetBankF(VRAM_F_MAIN_SPRITE);
    oamInit(&oamMain, SpriteMapping_1D_32, false);
    SPRITE_PALETTE[1] = RGB15(31, 31, 31);
    SPRITE_PALETTE[2] = RGB15(0, 0, 0);
    const char *word = "PAUSE";
    int x0 = (128 - 16 * 5) / 2;
    for (int pass = 0; pass < 2; pass++) // outline first, then the letters
        for (int k = 0; word[k]; k++) {
            const u8 *g = default_font_bin + (uint8_t)word[k] * 8;
            for (int j = 0; j < 8; j++)
                for (int i = 0; i < 8; i++) {
                    if (!(g[j] >> i & 1))
                        continue;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            int x = x0 + k * 16 + i * 2 + dx, y = 8 + j * 2 + dy;
                            if (pass == 1) {
                                buf[y][x] = 1;
                                continue;
                            }
                            for (int oy = -1; oy <= 1; oy++)
                                for (int ox = -1; ox <= 1; ox++)
                                    buf[y + oy][x + ox] = 2;
                        }
                }
        }
    for (int sp = 0; sp < 2; sp++) {
        pause_gfx[sp] = oamAllocateGfx(&oamMain, SpriteSize_64x32, SpriteColorFormat_16Color);
        static uint8_t tiles[64 * 32 / 2];
        for (int ty = 0; ty < 4; ty++)
            for (int tx = 0; tx < 8; tx++)
                for (int r = 0; r < 8; r++)
                    for (int c = 0; c < 8; c += 2) {
                        int x = sp * 64 + tx * 8 + c, y = ty * 8 + r;
                        tiles[(ty * 8 + tx) * 32 + r * 4 + c / 2] = buf[y][x] | buf[y][x + 1] << 4;
                    }
        dmaCopy(tiles, pause_gfx[sp], sizeof(tiles));
    }
}

static void pause_show(int on)
{
    if (on == pause_shown || !pause_gfx[0])
        return;
    pause_shown = on;
    for (int sp = 0; sp < 2; sp++)
        oamSet(&oamMain, sp, 64 + sp * 64, 80, 0, 0, SpriteSize_64x32, SpriteColorFormat_16Color,
               pause_gfx[sp], -1, false, !on, false, false, false);
    oamUpdate(&oamMain);
}

void menu_init(void)
{
    pause_init();
    uint32_t size;
    data = asset_load("nitro:/menu/items.bin", &size);
    if (data && *(uint32_t *)data == ITEMS_MAGIC) {
        num_icons = *(uint16_t *)(data + 4);
        num_names = *(uint16_t *)(data + 6);
        ui = data + 8;
        icons = data + 20 + 512;
        const uint16_t *p = (const uint16_t *)(icons + num_icons * ICON_W * ICON_H);
        portrait_w = p[0];
        portrait_h = p[1];
        portrait = (const uint8_t *)(p + 2);
        name_offs = (const uint16_t *)(portrait + portrait_w * portrait_h);
        names = (const char *)(name_offs + num_names);
    }
    pictures = asset_load("nitro:/menu/pictures.bin", NULL);
    if (pictures && *(uint32_t *)pictures == PICTURES_MAGIC) {
        num_pictures = *(uint16_t *)(pictures + 4);
        picture_w = *(uint16_t *)(pictures + 6);
        picture_h = *(uint16_t *)(pictures + 8);
    } else if (pictures) {
        asset_free(pictures);
        pictures = NULL;
    }
    show_screen();
}

void menu_debug(int on)
{
    debug_mode = on;
    if (on) {
        videoSetModeSub(MODE_0_2D);
        vramSetBankH(VRAM_H_SUB_BG);
        consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 4, 0, false, true);
        consoleClear();
    } else {
        show_screen();
    }
}

int menu_debug_on(void) { return debug_mode; }

void menu_message(const char *t)
{
    message = t;
    last_hp = -9999; // redraw
}

int menu_message_open(void) { return message != NULL; }

// ---------------------------------------------------------------- choices
#define CHOICE_Y 56                    // first line
#define CHOICE_H 11                    // line height

static void draw_choice(const char *title, const char *const *lines, int count, int sel)
{
    memset(fb, col(C_BLACK), sizeof(fb));
    frame(4, 4, 248, 46, C_GREY);
    text_wrapped(8, 8, 48, title, C_WHITE);
    for (int i = 0; i < count; i++) {
        int y = CHOICE_Y + i * CHOICE_H;
        if (i == sel)
            frame(4, y - 2, 248, CHOICE_H, C_YELLOW);
        text(8, y, lines[i], i == sel ? C_WHITE : C_LIGHT);
    }
    dirty = 1;
}

int menu_choose(const char *title, const char *const *lines, int count, int start, int can_cancel,
                void (*frame_fn)(void))
{
    int sel = start < 0 || start >= count ? 0 : start, answer = -2;
    pause_show(0);
    draw_choice(title, lines, count, sel);
    while (answer == -2) {
        scanKeys();
        uint32_t down = keysDown();
        int moved = 0;
        if (down & KEY_UP)
            sel = (sel + count - 1) % count, moved = 1;
        if (down & KEY_DOWN)
            sel = (sel + 1) % count, moved = 1;
        if (down & KEY_TOUCH) {
            touchPosition t;
            touchRead(&t);
            int i = (t.py - CHOICE_Y + 2) / CHOICE_H;
            if (t.py >= CHOICE_Y - 2 && i >= 0 && i < count) {
                sel = i;
                answer = i;
            }
        }
        if (down & KEY_A)
            answer = sel;
        if ((down & KEY_B) && can_cancel)
            answer = -1;
        if (moved)
            game_menu_sound(SND_CURSOR);
        if (answer >= 0)
            game_menu_sound(SND_SELECT);
        else if (answer == -1)
            game_menu_sound(SND_BACK);
        draw_choice(title, lines, count, sel);
        if (dirty && bg >= 0) {
            DC_FlushRange(fb, sizeof(fb));
            dmaCopy(fb, bgGetGfxPtr(bg), sizeof(fb));
            dirty = 0;
        }
        frame_fn();
    }
    last_hp = -9999; // the status screen again
    return answer;
}

void menu_picture(int item)
{
    picture = item;
    last_hp = -9999;
}

void menu_focus(int on)
{
    focus = on;
    row = 0;
    last_hp = -9999;
}

int menu_has_focus(void) { return focus; }

static void set_info(int slot)
{
    if (slot >= 0 && inventory[slot].item)
        snprintf(info, sizeof(info), "%s", item_name(inventory[slot].item));
    else
        info[0] = 0;
}

// ---------------------------------------------------------------- combine
// Weapon + its ammunition loads it up to the magazine size; herbs mix.
#define ITEM_GREEN 0x26
#define ITEM_RED 0x27
#define ITEM_BLUE 0x28
static const struct { uint8_t weapon, ammo, capacity; } loads[] = {
    { 0x02, 0x14, 18 }, { 0x03, 0x14, 13 }, { 0x04, 0x14, 18 },   // hand guns
    { 0x05, 0x16, 8 }, { 0x06, 0x16, 8 },                         // magnums
    { 0x07, 0x15, 5 }, { 0x08, 0x15, 7 },                         // shotguns
    { 0x09, 0x18, 6 }, { 0x0A, 0x19, 6 }, { 0x0B, 0x1A, 6 },      // grenade launcher
    { 0x0C, 0x1D, 18 }, { 0x0E, 0x1C, 100 }, { 0x0F, 0x1B, 100 }, { 0x10, 0x17, 100 },
};
// mixed herbs 0x29.. : G+G, G+R, G+B, G+G+G, G+G+B, G+R+B
static const uint8_t mixes[][3] = {
    { ITEM_GREEN, ITEM_GREEN, 0x29 }, { ITEM_GREEN, ITEM_RED, 0x2A }, { ITEM_GREEN, ITEM_BLUE, 0x2B },
    { 0x29, ITEM_GREEN, 0x2C }, { 0x29, ITEM_BLUE, 0x2D }, { 0x2B, ITEM_GREEN, 0x2D },
    { 0x2A, ITEM_BLUE, 0x2E }, { 0x2B, ITEM_RED, 0x2E },
};

// the ammunition slot for the weapon in hand, -1 if none (FUN_80065708)
static int spare_ammo(int *capacity)
{
    if (inventory_equipped < 0)
        return -1;
    int weapon = inventory[inventory_equipped].item;
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        if (loads[i].weapon != weapon)
            continue;
        for (int s = 0; s < INV_SLOTS; s++)
            if (inventory[s].item == loads[i].ammo && inventory[s].count) {
                *capacity = loads[i].capacity;
                return s;
            }
    }
    return -1;
}

int inventory_spare_ammo(void)
{
    int capacity;
    return spare_ammo(&capacity) >= 0;
}

// FUN_80065780: fills the weapon in hand from its ammunition
void inventory_reload(void)
{
    int capacity, s = spare_ammo(&capacity);
    if (s < 0)
        return;
    InvSlot *w = &inventory[inventory_equipped], *m = &inventory[s];
    int take = capacity - w->count;
    if (take > m->count)
        take = m->count;
    if (take <= 0)
        return;
    w->count += take;
    m->count -= take;
    if (!m->count)
        m->item = 0;
}

static int combine(int a, int b)
{
    InvSlot *x = &inventory[a], *y = &inventory[b];
    if (a == b || !x->item || !y->item)
        return 0;
    for (unsigned i = 0; i < sizeof(loads) / sizeof(loads[0]); i++) {
        InvSlot *w = x->item == loads[i].weapon ? x : y->item == loads[i].weapon ? y : NULL;
        InvSlot *m = w == x ? y : x;
        if (!w || m->item != loads[i].ammo)
            continue;
        int take = loads[i].capacity - w->count;
        if (take <= 0) {
            snprintf(info, sizeof(info), "%s is fully loaded.", item_name(w->item));
            return 0;
        }
        if (take > m->count)
            take = m->count;
        w->count += take;
        m->count -= take;
        if (!m->count)
            m->item = 0;
        snprintf(info, sizeof(info), "%s loaded.", item_name(w->item));
        game_menu_sound(SND_RELOAD);
        return 1;
    }
    for (unsigned i = 0; i < sizeof(mixes) / sizeof(mixes[0]); i++) {
        if (!((x->item == mixes[i][0] && y->item == mixes[i][1]) ||
              (y->item == mixes[i][0] && x->item == mixes[i][1])))
            continue;
        x->item = mixes[i][2];
        x->count = 0;
        y->item = 0;
        y->count = 0;
        if (inventory_equipped == b)
            inventory_equipped = -1;
        snprintf(info, sizeof(info), "Mixed the herbs.");
        game_menu_sound(SND_SELECT);
        return 1;
    }
    snprintf(info, sizeof(info), "You cannot combine these.");
    return 0;
}

static void command(int k)
{
    InvSlot *s = &inventory[sel];
    if (!s->item) {
        game_menu_sound(SND_NO);
        return;
    }
    switch (k) {
    case CMD_EQUIP:
        if (is_weapon(s->item)) {
            inventory_equipped = inventory_equipped == sel ? -1 : sel;
            snprintf(info, sizeof(info), inventory_equipped == sel ? "%s equipped." : "%s put away.",
                     item_name(s->item));
        } else {
            snprintf(info, sizeof(info), "You cannot equip that.");
            game_menu_sound(SND_NO);
            return;
        }
        break;
    case CMD_USE:
        snprintf(info, sizeof(info), "%s: nothing happens here.", item_name(s->item));
        break;
    case CMD_CHECK:
        snprintf(info, sizeof(info), "%s.", item_name(s->item));
        menu_picture(s->item);
        break;
    case CMD_COMBINE:
        combine_from = sel;
        snprintf(info, sizeof(info), "Combine %s with what?", item_name(s->item));
        break;
    }
    game_menu_sound(SND_SELECT);
}

// second item of a COMBINE chosen
static void combine_with(int slot)
{
    if (!combine(combine_from, slot))
        game_menu_sound(SND_NO);
    combine_from = -1;
}

static int touched_slot(int tx, int ty)
{
    for (int i = 0; i < INV_SLOTS; i++) {
        int x, y;
        slot_pos(i, &x, &y);
        if (tx >= x && tx < x + 48 && ty >= y && ty < y + 36)
            return i;
    }
    return -1;
}

int menu_frame(uint32_t down, int hp)
{
    if (debug_mode)
        return 0;
    int changed = 0;
    if (picture && !message && down) { // a CHECK picture stays until the next input
        picture = 0;
        changed = 1;
    }
    if (message) {
        if (down & (KEY_A | KEY_B | KEY_TOUCH)) {
            message = NULL;
            picture = 0;
            changed = 1;
        }
    } else if (down & KEY_TOUCH) {
        touchPosition t;
        touchRead(&t);
        int s = touched_slot(t.px, t.py);
        if (s >= 0) {
            if (!focus)
                focus = 1;
            row = 0;
            sel = s;
            if (combine_from >= 0) {
                combine_with(s);
            } else {
                set_info(sel);
                game_menu_sound(SND_CURSOR);
            }
        } else if (t.py >= 134 && t.py < 146 && focus) {
            cmd_sel = (t.px - 6) / 62;
            if (cmd_sel >= 0 && cmd_sel < NUM_CMDS)
                command(cmd_sel);
        } else if (t.py < 54) {
            focus = !focus; // tap the top to leave (or enter) the menu
            combine_from = -1;
            game_menu_sound(focus ? SND_SELECT : SND_BACK);
        }
        changed = 1;
    } else if (focus) {
        int keep_info = 0;     // a command's answer stays in the text box
        if (down & (KEY_START | KEY_B)) {
            if (combine_from >= 0 && (down & KEY_B))
                combine_from = -1;
            else if (row == 1 && (down & KEY_B))
                row = 0;
            else
                focus = 0, combine_from = -1;
            game_menu_sound(SND_BACK);
        }
        if (down & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN))
            game_menu_sound(SND_CURSOR);
        if (row == 0) {
            if (down & KEY_LEFT)
                sel = (sel + INV_SLOTS - 1) % INV_SLOTS;
            if (down & KEY_RIGHT)
                sel = (sel + 1) % INV_SLOTS;
            if (down & (KEY_UP | KEY_DOWN))
                sel ^= 4;
            if ((down & KEY_A) && combine_from >= 0) {
                combine_with(sel);
                keep_info = 1;
            } else if (down & KEY_A) {
                row = 1;
                cmd_sel = 0;
                game_menu_sound(SND_SELECT);
            }
        } else {
            if (down & KEY_LEFT)
                cmd_sel = (cmd_sel + NUM_CMDS - 1) % NUM_CMDS;
            if (down & KEY_RIGHT)
                cmd_sel = (cmd_sel + 1) % NUM_CMDS;
            if (down & KEY_A) {
                command(cmd_sel);
                row = 0;
                keep_info = 1;
            }
        }
        if (down && !keep_info && combine_from < 0)
            set_info(sel);
        if (down)
            changed = 1;
    } else if (down & KEY_START) {
        focus = 1;
        set_info(sel);
        changed = 1;
        game_menu_sound(SND_SELECT);
    }
    // the game changes the inventory too (shots, pickups): redraw then
    uint32_t inv = inventory_equipped;
    for (int i = 0; i < INV_SLOTS; i++)
        inv = inv * 31 + (inventory[i].item << 16 | inventory[i].count);
    if (inv != last_inv)
        changed = 1, last_inv = inv;
    if (changed || hp != last_hp) {
        redraw(hp);
        last_hp = hp;
    } else {
        draw_condition(hp);
    }
    ecg_phase += hp < 21 ? 3 : 2;
    if (dirty && bg >= 0) {
        DC_FlushRange(fb, sizeof(fb));
        dmaCopy(fb, bgGetGfxPtr(bg), sizeof(fb));
        dirty = 0;
    }
    pause_show(focus && !message);
    return focus || message;
}
