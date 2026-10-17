// re2ds - the status screen on the lower DS screen: condition, the weapon in
// hand, Leon's 8 item slots with the PS1 icons (menu/items.bin), commands
// and the text box for messages. Touch or buttons; play pauses while it has
// the focus.
#pragma once

#include <stdint.h>

void menu_init(void);
const char *item_name(int item);     // "" if unknown
// once per DS frame. hp: Leon's health. Returns 1 while the menu has the
// focus (the game is paused then).
int menu_frame(uint32_t down, int hp);
void menu_focus(int on);
int menu_has_focus(void);
// messages (room texts, items) in the text box; A / B / touch closes them
void menu_message(const char *text);
int menu_message_open(void);
// A choice over the whole lower screen (yes / no, save slots): up / down and
// A or touch pick a line, B cancels if allowed (-1). Runs until answered,
// calling frame_fn once per DS frame so the upper screen keeps going.
int menu_choose(const char *title, const char *const *lines, int count, int start, int can_cancel,
                void (*frame_fn)(void));
// show the item's picture (as on the PS1 when picking it up) until the
// message closes or, without a message, until the next input
void menu_picture(int item);
// ammunition for the weapon in hand in the inventory; load it (the reload)
int inventory_spare_ammo(void);
void inventory_reload(void);
// debug mode: the lower screen shows the text console instead
void menu_debug(int on);
int menu_debug_on(void);
