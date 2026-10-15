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
// debug mode: the lower screen shows the text console instead
void menu_debug(int on);
int menu_debug_on(void);
