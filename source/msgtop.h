// re2ds - the messages on the upper screen, in the PS1's own font
// (menu/font.bin, tools/font2ds.py): white with a shadow along the bottom
// of the picture, item names (\x01 ... \x01) and the chosen answer in green.
#pragma once

void msgtop_init(void);                 // after the main screen's sprites are set up
// the page [s, end) of a message; choice: YES / NO under it, answer 0 yes 1 no
void msgtop_show(const char *s, const char *end, int choice, int answer);
void msgtop_hide(void);
