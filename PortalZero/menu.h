#ifndef MENU_H
#define MENU_H

#include <stdbool.h>

#define MENU_ROWS 5   // visible entries at once

void initScreen(void);
void drawMessage(const char *title, const char *l1, const char *l2, const char *l3);
void drawSdError(int fresult);   // FatFs error code for diagnostics
// is_placed(idx): is the figure in row idx currently on the portal?
void drawBrowser(int sel, int top, const char *footer, bool (*is_placed)(int idx));

#endif
