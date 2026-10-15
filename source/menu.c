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

enum { C_BLACK, C_DARK, C_GREY, C_LIGHT, C_WHITE, C_GREEN, C_YELLOW, C_RED, C_DIM_GREEN, C_DIM_YELLOW, C_DIM_RED };
enum { CMD_EQUIP, CMD_USE, CMD_CHECK, CMD_COMBINE, NUM_CMDS };
static const char *const cmd_names[NUM_CMDS] = { "EQUIP", "USE", "CHECK", "COMBINE" };

extern const u8 default_font_bin[];

static uint8_t fb[256 * 192];
static int dirty, bg = -1, debug_mode;
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

// word-wrapped text in the box at the bottom (5 lines of 30 characters)
static void text_box(const char *s, int c)
{
    fill(4, 150, 248, 40, C_BLACK);
    frame(4, 150, 248, 40, C_GREY);
    char line[31];
    int y = 153;
    while (*s && y < 188) {
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
        text(8, y, line, c);
        s += len;
        while (*s == ' ' || *s == '\n')
            s++;
        y += 9;
    }
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
    int c = focus && row == 0 && sel == i ? C_YELLOW : i == inventory_equipped ? C_GREEN : C_DARK;
    fill(x, y, 48, 36, C_BLACK);
    frame(x, y, 48, 36, c);
    if (inventory[i].item) {
        icon(x + 4, y + 3, inventory[i].item);
        int item = inventory[i].item;
        if (item >= 0x02) { // the knife has no count
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

static void redraw(int hp)
{
    memset(fb, col(C_BLACK), sizeof(fb));
    draw_portrait();
    draw_condition(hp);
    draw_weapon();
    for (int i = 0; i < INV_SLOTS; i++)
        draw_slot(i);
    draw_commands();
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

static void command(int k)
{
    InvSlot *s = &inventory[sel];
    if (!s->item)
        return;
    switch (k) {
    case CMD_EQUIP:
        if (is_weapon(s->item)) {
            inventory_equipped = inventory_equipped == sel ? -1 : sel;
            snprintf(info, sizeof(info), inventory_equipped == sel ? "%s equipped." : "%s put away.",
                     item_name(s->item));
        } else {
            snprintf(info, sizeof(info), "You cannot equip that.");
        }
        break;
    case CMD_USE:
        snprintf(info, sizeof(info), "%s: nothing happens here.", item_name(s->item));
        break;
    case CMD_CHECK:
        snprintf(info, sizeof(info), "%s.", item_name(s->item));
        break;
    case CMD_COMBINE:
        snprintf(info, sizeof(info), "Combining comes later.");
        break;
    }
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
    if (message) {
        if (down & (KEY_A | KEY_B | KEY_TOUCH)) {
            message = NULL;
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
            set_info(sel);
        } else if (t.py >= 134 && t.py < 146 && focus) {
            cmd_sel = (t.px - 6) / 62;
            if (cmd_sel >= 0 && cmd_sel < NUM_CMDS)
                command(cmd_sel);
        } else if (t.py < 54) {
            focus = !focus; // tap the top to leave (or enter) the menu
        }
        changed = 1;
    } else if (focus) {
        if (down & (KEY_START | KEY_B)) {
            if (row == 1 && (down & KEY_B))
                row = 0;
            else
                focus = 0;
        }
        if (row == 0) {
            if (down & KEY_LEFT)
                sel = (sel + INV_SLOTS - 1) % INV_SLOTS;
            if (down & KEY_RIGHT)
                sel = (sel + 1) % INV_SLOTS;
            if (down & (KEY_UP | KEY_DOWN))
                sel ^= 4;
            if (down & KEY_A) {
                row = 1;
                cmd_sel = 0;
            }
        } else {
            if (down & KEY_LEFT)
                cmd_sel = (cmd_sel + NUM_CMDS - 1) % NUM_CMDS;
            if (down & KEY_RIGHT)
                cmd_sel = (cmd_sel + 1) % NUM_CMDS;
            if (down & KEY_A) {
                command(cmd_sel);
                row = 0;
            }
        }
        if (down)
            set_info(sel), changed = 1;
    } else if (down & KEY_START) {
        focus = 1;
        set_info(sel);
        changed = 1;
    }
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
