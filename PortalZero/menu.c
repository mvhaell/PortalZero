// Screen output for a 0.96" SSD1306 OLED (128x64, 21 characters x 8 lines)
//
// Layout:
//   y = 0       header (current folder)
//   y = 9       separator line
//   y = 11..50  5 entries (ROW(0) to ROW(4))
//   y = 54      separator line
//   y = 56      footer (status or message)
//
// Entries: "< back", "> folder", "* figure on the portal", heart = favorite, "  figure"
#include <stdio.h>
#include <string.h>
#include "menu.h"
#include "oled.h"
#include "figures.h"

#define ROW(n) (11 + 8 * (n))
#define FOOTER_LINE 54
#define FOOTER_Y    56

// Prepare UTF-8 names for the 5x7 font: transliterate umlauts/accents, anything else becomes '?'
static void to_display(char *out, size_t size, const char *in, size_t max_chars) {
    size_t k = 0, chars = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && chars < max_chars && k + 3 < size) {
        if (*p < 0x80) { out[k++] = (char)*p++; chars++; continue; }
        const char *rep = "?";
        if (p[0] == 0xC3 && p[1]) {
            switch (p[1]) {
                case 0xA4: rep = "ae"; break;  case 0x84: rep = "Ae"; break;
                case 0xB6: rep = "oe"; break;  case 0x96: rep = "Oe"; break;
                case 0xBC: rep = "ue"; break;  case 0x9C: rep = "Ue"; break;
                case 0x9F: rep = "ss"; break;
                case 0xA9: case 0xA8: case 0xAA: rep = "e"; break;   // e with accent
                case 0xA1: case 0xA0: case 0xA2: rep = "a"; break;   // a with accent
            }
        }
        // skip the whole UTF-8 character
        int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : (*p >= 0xC0) ? 2 : 1;
        for (int i = 0; i < len && *p; i++) p++;
        for (const char *r = rep; *r && chars < max_chars && k + 1 < size; r++) { out[k++] = *r; chars++; }
    }
    out[k] = '\0';
}

static void strip_ext(char *name) {
    char *dot = strrchr(name, '.');
    if (dot && dot != name) *dot = '\0';
}

static void drawFrame(const char *title, bool inverted, const char *footer){
    oled_clear();
    char t[24];
    to_display(t, sizeof(t), title, 21);
    oled_text_center(0, t, inverted);
    oled_hline(9);
    oled_hline(FOOTER_LINE);
    oled_text_center(FOOTER_Y, footer, false);
}

void initScreen(void){
    oled_init();
}

void drawMessage(const char *title, const char *l1, const char *l2, const char *l3){
    drawFrame(title, false, "");
    if (l1) oled_text_center(ROW(1), l1, false);
    if (l2) oled_text_center(ROW(2), l2, false);
    if (l3) oled_text_center(ROW(3), l3, false);
    oled_show();
}

// fresult = error code from f_mount (FatFs): 1 = read error, 3 = no response,
// 13 = no FAT file system
void drawSdError(int fresult){
    char code[24];
    drawFrame("SD card error", true, "SEL = retry");
    switch (fresult) {
        case 3:   // FR_NOT_READY: card does not respond at all
            oled_text(0, ROW(0), "Card not responding", false);
            oled_text(0, ROW(1), "- Card inserted?", false);
            oled_text(0, ROW(2), "- Check CS/SCK/MOSI/", false);
            oled_text(0, ROW(3), "  MISO/VCC/GND wires", false);
            break;
        case 1:   // FR_DISK_ERR: card responds, but data arrives garbled
            oled_text(0, ROW(0), "Read error", false);
            oled_text(0, ROW(1), "- MISO/MOSI swapped?", false);
            oled_text(0, ROW(2), "  or loose contact?", false);
            oled_text(0, ROW(3), "- Try another card", false);
            break;
        case 13:  // FR_NO_FILESYSTEM: card OK, wrong format
            oled_text(0, ROW(0), "Card OK, but format", false);
            oled_text(0, ROW(1), "not recognized.", false);
            oled_text(0, ROW(2), "Format it as FAT32", false);
            oled_text(0, ROW(3), "or exFAT on a PC.", false);
            break;
        default:
            oled_text(0, ROW(0), "SD card not readable", false);
            oled_text(0, ROW(1), "- Card inserted?", false);
            oled_text(0, ROW(2), "- Check the wiring", false);
            break;
    }
    snprintf(code, sizeof(code), "(Code %d)", fresult);
    oled_text(0, ROW(4), code, false);
    oled_show();
}

void drawBrowser(int sel, int top, const char *footer, bool (*is_placed)(int idx)){
    drawFrame(browser_dir_name(), false, footer);
    int n = browser_count();

    for (int r = 0; r < MENU_ROWS && top + r < n; r++) {
        int idx = top + r;
        char raw[ENTRY_NAME_LEN], name[24], line[24];
        char mark;
        snprintf(raw, sizeof(raw), "%s", browser_name(idx));

        switch (browser_type(idx)) {
            case ENTRY_UP:  mark = '<'; strcpy(raw, "back"); break;
            case ENTRY_FAV:
            case ENTRY_DIR: mark = '>'; break;
            default:        // '*' = on the portal, heart = favorite
                mark = is_placed(idx) ? '*' : (browser_is_fav(idx) ? '\x7F' : ' ');
                strip_ext(raw);
                break;
        }
        to_display(name, sizeof(name), raw, 19);
        snprintf(line, sizeof(line), "%c%s", mark, name);

        bool selected = (idx == sel);
        if (selected) oled_fill_rect(0, ROW(r), OLED_WIDTH, 8, true);   // selection bar
        oled_text(0, ROW(r), line, selected);
        // scroll hint on the right if the list continues above/below
        if (r == 0 && top > 0)                         oled_text(122, ROW(r), "^", selected);
        if (r == MENU_ROWS - 1 && top + MENU_ROWS < n) oled_text(122, ROW(r), "v", selected);
    }

    // hint if the folder has neither figures nor subfolders
    int shown = n < MENU_ROWS ? n : MENU_ROWS;
    bool only_up = (n == 0) || (n == 1 && browser_type(0) == ENTRY_UP);
    if (only_up && shown + 2 <= MENU_ROWS) {
        oled_text(0, ROW(shown),     " No figures here", false);
        oled_text(0, ROW(shown + 1), " (.sky / .nfc)", false);
    }
    oled_show();
}
