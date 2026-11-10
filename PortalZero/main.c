// Portal Zero - Skylanders portal emulator for the Wii
// Waveshare RP2040-Zero + 0.96" I2C OLED + SD card + 3 buttons
//
// Controls:
//   UP / DOWN:       select an entry (hold = scroll fast)
//   SEL short:       open folder / go back / place or remove a figure
//   SEL long (1 s):  on a figure: add to / remove from favorites
//                    on a folder/"back": reload the SD card (e.g. after swapping cards)
//
// The last placed figures are remembered in a hidden file (/.portalzero_last) and put
// back on the portal automatically on the next start.
//
// Flash mode (like the BOOTSEL button) without opening the case - 4 ways:
//   1. hold SEL while plugging in  (always works, even without display/SD card)
//   2. hold UP + DOWN together for 3 s while running
//   3. put a file named BOOTSEL.txt on the SD card (deleted on start)
//   4. from the PC with tools/portalzero_flash.py
//
// Buttons: UP = GP8, SEL = GP7, DOWN = GP6, each wired to GND.
// Internal pull-ups are used - no resistors needed.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/critical_section.h"
#include "hardware/gpio.h"
#include "bsp/board.h"
#include "tusb.h"

#include "ff.h"
#include "diskio.h"
#include "hw_config.h"

#include "menu.h"
#include "portal.h"
#include "figures.h"

// Buttons (RP2040-Zero): each button between its GPIO and GND
#define BUTTON_UP_PIN       8
#define BUTTON_SELECT_PIN   7
#define BUTTON_DOWN_PIN     6

#define DEBOUNCE_MS      30     // ignore contact bounce
#define REPEAT_DELAY_MS  450    // UP/DOWN held: start repeating after this
#define REPEAT_RATE_MS   110
#define LONG_PRESS_MS    1000   // hold SEL this long = long press
#define USB_FIRST_MS     500    // read the SD card once USB is up (wait at most this long)
#define SAVE_IDLE_MS     1500   // save once the game has stopped writing for this long
#define SAVE_RETRY_MS    5000   // after a save error, retry no earlier than this
#define MESSAGE_MS       1500   // footer message duration
#define FLASH_COMBO_MS   3000   // hold UP + DOWN this long = flash mode

// ---------------------------------------------------------------------------
// Time and messages
static uint32_t millis(void) {
    return to_ms_since_boot(get_absolute_time());
}

static char     message[24];
static uint32_t message_until;

static void show_message(const char *text) {
    snprintf(message, sizeof(message), "%s", text);
    message_until = millis() + MESSAGE_MS;
}

// ---------------------------------------------------------------------------
// Buttons (debounced)
typedef struct {
    uint     pin;          // input with pull-up
    bool     pressed;
    bool     long_fired;
    uint32_t changed;
    uint32_t pressed_at;
    uint32_t next_repeat;
} button_t;

static button_t btn_up     = { .pin = BUTTON_UP_PIN };
static button_t btn_select = { .pin = BUTTON_SELECT_PIN };
static button_t btn_down   = { .pin = BUTTON_DOWN_PIN };

static void init_button_pin(uint pin) {
    gpio_init(pin);                              // input with pull-up, button pulls to GND
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

static void init_button(button_t *b) {
    init_button_pin(b->pin);
}

// true if the debounced state has changed
static bool button_update(button_t *b, uint32_t now) {
    bool raw = !gpio_get(b->pin);
    if (raw != b->pressed && now - b->changed >= DEBOUNCE_MS) {
        b->pressed = raw;
        b->changed = now;
        return true;
    }
    return false;
}

// UP/DOWN: fires on press and then repeatedly while held
static bool nav_event(button_t *b, uint32_t now) {
    if (button_update(b, now) && b->pressed) {
        b->next_repeat = now + REPEAT_DELAY_MS;
        return true;
    }
    if (b->pressed && (int32_t)(now - b->next_repeat) >= 0) {
        b->next_repeat = now + REPEAT_RATE_MS;
        return true;
    }
    return false;
}

typedef enum { SEL_NONE, SEL_SHORT, SEL_LONG } sel_event_t;

// SEL: short = on release, long = as soon as LONG_PRESS_MS is reached
static sel_event_t select_event(button_t *b, uint32_t now) {
    if (button_update(b, now)) {
        if (b->pressed) {
            b->pressed_at = now;
            b->long_fired = false;
        } else if (!b->long_fired) {
            return SEL_SHORT;
        }
    }
    if (b->pressed && !b->long_fired && now - b->pressed_at >= LONG_PRESS_MS) {
        b->long_fired = true;
        return SEL_LONG;
    }
    return SEL_NONE;
}

// ---------------------------------------------------------------------------
// USB runs entirely on the second CPU core (core 1).
// This way the Wii is always served immediately - even while core 0 is writing
// to the SD card or updating the display.
// The portal state is used by both cores and is therefore protected by a lock.
static critical_section_t portal_cs;
#define P_LOCK()   critical_section_enter_blocking(&portal_cs)
#define P_UNLOCK() critical_section_exit(&portal_cs)

static void core1_usb_main(void) {
    board_init();
    tud_init(0);
    while (true) {
        tud_task();
        if (tud_mounted() && tud_hid_ready()) {
            uint8_t report[PORTAL_REPORT_SIZE];
            P_LOCK();
            portal_next_report(report);
            P_UNLOCK();
            tud_hid_report(0, report, sizeof(report));
        }
    }
}

// ---------------------------------------------------------------------------
// Figures on the portal: which slot belongs to which file
static char     slot_path[PORTAL_SLOTS][FIG_PATH_LEN];
static uint32_t slot_order[PORTAL_SLOTS];        // order in which figures were placed
static uint32_t place_counter;
static uint32_t save_blocked_until;

static int slot_of_path(const char *path) {
    int found = -1;
    P_LOCK();
    for (int i = 0; i < PORTAL_SLOTS && found < 0; i++)
        if (portal_slot_used(i) && strcmp(slot_path[i], path) == 0) found = i;
    P_UNLOCK();
    return found;
}

static bool entry_placed(int idx) {
    char path[FIG_PATH_LEN];
    browser_path(idx, path, sizeof(path));
    return slot_of_path(path) >= 0;
}

// Save the game progress of one slot. The data is copied under the lock and then
// written without it - USB keeps running undisturbed meanwhile.
// Returns true = saved or nothing to do.
static bool save_slot(int slot) {
    static uint8_t copy[PORTAL_FIGURE_SIZE];
    uint32_t seq = 0;
    P_LOCK();
    bool dirty = portal_slot_dirty(slot);
    if (dirty) {
        memcpy(copy, portal_slot_data(slot), PORTAL_FIGURE_SIZE);
        seq = portal_slot_seq(slot);
    }
    P_UNLOCK();
    if (!dirty) return true;
    if (!figure_save(slot_path[slot], copy)) return false;
    P_LOCK();
    portal_clear_dirty_if(slot, seq);            // if the game wrote again meanwhile, it stays "changed"
    P_UNLOCK();
    return true;
}

// Save game progress automatically once the game has stopped writing for a while
static void save_task(uint32_t now) {
    if ((int32_t)(save_blocked_until - now) > 0) return;
    P_LOCK();
    int slot = portal_dirty_slot(now, SAVE_IDLE_MS);
    P_UNLOCK();
    if (slot >= 0 && !save_slot(slot)) {
        save_blocked_until = now + SAVE_RETRY_MS;
        show_message("Error while saving");
    }
}

static void save_all(void) {
    for (int i = 0; i < PORTAL_SLOTS; i++) save_slot(i);
}

// Remove a figure from the portal; save the progress before (and, in case the
// game is writing at that very moment, after) removing it
static bool remove_figure(int slot) {
    static uint8_t last[PORTAL_FIGURE_SIZE];
    if (!save_slot(slot)) return false;
    P_LOCK();
    bool dirty = portal_slot_dirty(slot);
    if (dirty) memcpy(last, portal_slot_data(slot), PORTAL_FIGURE_SIZE);
    portal_remove(slot);
    P_UNLOCK();
    if (dirty) figure_save(slot_path[slot], last);
    return true;
}

// ---------------------------------------------------------------------------
// Remember the last placed figures
//
// Remembered is whatever is currently on the portal (most recently placed last).
// When the last figure is removed it stays remembered - so it comes back on the
// next start even if it was taken off before switching off.
static void remember_figures(void) {
    static char list[PORTAL_SLOTS][FIG_PATH_LEN];
    int idx[PORTAL_SLOTS], n = 0;
    P_LOCK();
    for (int i = 0; i < PORTAL_SLOTS; i++) if (portal_slot_used(i)) idx[n++] = i;
    P_UNLOCK();
    if (n == 0) return;                          // portal empty: keep remembering the last figure
    for (int a = 1; a < n; a++)                  // sort by placing order
        for (int b = a; b > 0 && slot_order[idx[b]] < slot_order[idx[b - 1]]; b--) {
            int t = idx[b]; idx[b] = idx[b - 1]; idx[b - 1] = t;
        }
    for (int i = 0; i < n; i++) strcpy(list[i], slot_path[idx[i]]);
    last_save(list, n);
}

// Place a figure from a file on the portal. Returns: slot, -1 = invalid file, -2 = portal full
static int place_figure(const char *path, fig_result_t *result) {
    static uint8_t buf[PORTAL_FIGURE_SIZE];
    *result = figure_load(path, buf);
    if (*result == FIG_ERROR) return -1;
    P_LOCK();
    int slot = portal_place(buf);
    if (slot >= 0) {
        snprintf(slot_path[slot], FIG_PATH_LEN, "%s", path);
        slot_order[slot] = ++place_counter;
    }
    P_UNLOCK();
    return slot >= 0 ? slot : -2;
}

// Index of the browser entry with this path (0 if not found)
static int find_entry(const char *path) {
    char p[FIG_PATH_LEN];
    for (int i = 0; i < browser_count(); i++) {
        browser_path(i, p, sizeof(p));
        if (strcmp(p, path) == 0) return i;
    }
    return 0;
}

// On start: put the last figures back on the portal and open the folder of the last one.
// Returns the number of restored figures; *sel = selected browser entry
static int restore_figures(int *sel) {
    static char list[PORTAL_SLOTS][FIG_PATH_LEN];
    int n = last_load(list, PORTAL_SLOTS), restored = 0;
    const char *last = NULL;
    for (int i = 0; i < n; i++) {
        fig_result_t r;
        if (slot_of_path(list[i]) >= 0) continue;          // duplicate
        if (place_figure(list[i], &r) >= 0) { restored++; last = list[i]; }
    }
    if (!last) return 0;
    char dir[FIG_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s", last);
    char *slash = strrchr(dir, '/');
    if (slash == dir) dir[1] = '\0'; else if (slash) *slash = '\0';
    if (browser_open(dir)) *sel = find_entry(last);
    return restored;
}

// ---------------------------------------------------------------------------
// Flash mode without the BOOTSEL button.
// The board then shows up on the PC as drive RPI-RP2, just like with BOOTSEL.
static void enter_bootsel(bool sd_ready);

// Way 2: hold UP + DOWN together for 3 s while running
static void check_flash_combo(uint32_t now, bool sd_ready) {
    // only reads the state - it is updated by nav_event()
    static uint32_t both_since;
    static bool both;
    if (!(btn_down.pressed && btn_up.pressed)) { both = false; return; }
    if (!both) { both = true; both_since = now; return; }
    if (now - both_since < FLASH_COMBO_MS) return;
    enter_bootsel(sd_ready);
}

// Flash mode via a command from the PC (portalzero_flash.py). The Wii never sends this text.
static const char     BOOTSEL_MAGIC[] = "PORTALZERO-FLASH";
static volatile bool  bootsel_requested;

static void enter_bootsel(bool sd_ready) {
    if (sd_ready) save_all();                    // save game progress first
    drawMessage("Flash mode", "Drive RPI-RP2", "appears on the PC.", "Drag PortalZero.uf2");
    sleep_ms(300);
    reset_usb_boot(0, 0);                        // does not return
}

// Way 1: hold SEL while plugging in -> flash mode right away,
// before USB, display or SD card are started
static void check_boot_select(void) {
    init_button_pin(BUTTON_SELECT_PIN);
    sleep_ms(20);                                // let the pull-up settle
    for (int i = 0; i < 10; i++) {               // pressed continuously for 100 ms?
        if (gpio_get(BUTTON_SELECT_PIN)) return; // not pressed -> normal start
        sleep_ms(10);
    }
    reset_usb_boot(0, 0);
}

// Way 3: file BOOTSEL.txt on the SD card
static void check_bootsel_file(void) {
    if (f_stat("/BOOTSEL.txt", NULL) != FR_OK) return;
    // delete it first, otherwise the board would land in flash mode again after flashing
    if (f_unlink("/BOOTSEL.txt") != FR_OK) return;
    enter_bootsel(false);
}

// ---------------------------------------------------------------------------
// SD card
static sd_card_t *sd;

static FRESULT sd_result;                         // last error code, for the display

static bool mount_sd(void) {
    f_unmount(sd->pcName);
    sd->m_Status |= STA_NOINIT;                  // fully re-initialize the card every time
    sd_result = f_mount(&sd->fatfs, sd->pcName, 1);
    if (sd_result != FR_OK) return false;
    f_mkdir(FIGURES_DIR);                        // in case it does not exist yet
    fav_load();
    return true;
}

// (Re-)open a folder; if it no longer exists: Skylanders, otherwise the root folder
static void open_browser(const char *dir) {
    if (!browser_open(dir) && !browser_open(FIGURES_DIR)) browser_open("/");
}

// ---------------------------------------------------------------------------
int main(void) {
    // 1. SEL held while plugging in? -> flash mode
    check_boot_select();

    // 2. announce ourselves to the Wii as a Skylanders portal right away (USB on core 1)
    stdio_init_all();
    portal_init();
    critical_section_init(&portal_cs);
    multicore_launch_core1(core1_usb_main);
    uint32_t start = millis();

    // 3. buttons and display
    init_button(&btn_up);
    init_button(&btn_select);
    init_button(&btn_down);
    initScreen();
    drawMessage("Portal Zero", "Reading SD card...", NULL, NULL);

    // 4. read the SD card once USB is up (or after USB_FIRST_MS at the latest)
    while (!tud_mounted() && millis() - start < USB_FIRST_MS) tight_loop_contents();
    sd = sd_get_by_num(0);
    while (!mount_sd()) {
        drawSdError((int)sd_result);
        while (select_event(&btn_select, millis()) != SEL_SHORT) {
            button_update(&btn_down, millis());
            button_update(&btn_up, millis());
            check_flash_combo(millis(), false);
            if (bootsel_requested) enter_bootsel(false);
        }
        drawMessage("Portal Zero", "Reading SD card...", NULL, NULL);
    }
    check_bootsel_file();
    open_browser(FIGURES_DIR);

    int sel = 0, top = 0;
    bool redraw = true;
    char last_footer[24] = "";

    // restore the last placed figures
    int restored = restore_figures(&sel);
    if (restored == 1)     show_message("Last figure restored");
    else if (restored > 1) { char m[24]; snprintf(m, sizeof(m), "%d figures restored", restored); show_message(m); }

    while (true) {
        uint32_t now = millis();
        save_task(now);

        int n = browser_count();

        // --- UP / DOWN
        if (nav_event(&btn_down, now) && sel < n - 1) { sel++; redraw = true; }
        if (nav_event(&btn_up, now) && sel > 0)       { sel--; redraw = true; }
        check_flash_combo(now, true);
        if (bootsel_requested) enter_bootsel(true);

        // --- SEL
        sel_event_t ev = select_event(&btn_select, now);
        if (ev == SEL_SHORT && n > 0) {
            entry_type_t type = browser_type(sel);
            if (type == ENTRY_UP || type == ENTRY_DIR || type == ENTRY_FAV) {
                int new_sel;
                if (browser_enter(sel, &new_sel)) { sel = new_sel; top = 0; }
                else show_message("Folder error");
            } else {
                char path[FIG_PATH_LEN];
                browser_path(sel, path, sizeof(path));
                int slot = slot_of_path(path);
                if (slot >= 0) {
                    // remove - save the game progress first
                    if (remove_figure(slot)) { show_message("Figure removed"); remember_figures(); }
                    else                     show_message("Error while saving");
                } else {
                    fig_result_t r;
                    slot = place_figure(path, &r);
                    if (slot == -1)      show_message("Invalid file");
                    else if (slot == -2) show_message("Portal full (max 16)");
                    else {
                        show_message(r == FIG_INCOMPLETE ? "Warning: incomplete" : "Figure placed");
                        remember_figures();
                    }
                }
            }
            redraw = true;
        } else if (ev == SEL_LONG && n > 0 && browser_type(sel) == ENTRY_FIGURE) {
            // add / remove favorite
            char path[FIG_PATH_LEN];
            browser_path(sel, path, sizeof(path));
            int r = fav_toggle(path);
            if (r == 1)      show_message("Added to favorites");
            else if (r == 0) show_message("Favorite removed");
            else             show_message(fav_count() >= FAV_MAX ? "Favorites full (32)" : "Error while saving");
            if (r >= 0) {
                // rebuild the list ("Favorites" folder appears/disappears,
                // in the favorites view the removed figure disappears)
                browser_rescan();
                if (!browser_in_favorites()) sel = find_entry(path);
            }
            redraw = true;
        } else if (ev == SEL_LONG) {
            // reload the SD card
            drawMessage(browser_dir_name(), "Reading SD card...", NULL, NULL);
            save_all();
            char dir[FIG_PATH_LEN];
            snprintf(dir, sizeof(dir), "%s", browser_dir());
            if (mount_sd()) {
                open_browser(dir);
                show_message("SD card reloaded");
            } else {
                show_message("SD card missing!");
            }
            redraw = true;
        }

        // keep the selection in range and scroll it into view
        n = browser_count();
        if (sel >= n) sel = n > 0 ? n - 1 : 0;
        if (sel < top) top = sel;
        if (sel >= top + MENU_ROWS) top = sel - MENU_ROWS + 1;

        // --- footer: short message or status
        char footer[24];
        if ((int32_t)(message_until - now) > 0) {
            snprintf(footer, sizeof(footer), "%s", message);
        } else if (!tud_mounted()) {
            strcpy(footer, "Wii not connected");
        } else {
            P_LOCK();
            bool act = portal_is_active();
            int c = portal_count();
            P_UNLOCK();
            if (!act) strcpy(footer, "Waiting for game...");
            else snprintf(footer, sizeof(footer), "Active: %d %s", c, c == 1 ? "figure" : "figures");
        }
        if (strcmp(footer, last_footer) != 0) {
            strcpy(last_footer, footer);
            redraw = true;
        }

        if (redraw) {
            drawBrowser(sel, top, footer, entry_placed);
            redraw = false;
        }
    }
}

// ---------------------------------------------------------------------------
// TinyUSB callbacks

void tud_mount_cb(void) {}
void tud_umount_cb(void) {}
void tud_suspend_cb(bool remote_wakeup_en) { (void)remote_wakeup_en; }
void tud_resume_cb(void) {}

void tud_hid_report_complete_cb(uint8_t instance, uint8_t const *report, uint16_t len) {
    (void)instance; (void)report; (void)len;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}

// Commands from the Wii arrive here (SET_REPORT or OUT endpoint)
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize) {
    (void)instance; (void)report_id; (void)report_type;
    if (bufsize >= sizeof(BOOTSEL_MAGIC) - 1 &&
        memcmp(buffer, BOOTSEL_MAGIC, sizeof(BOOTSEL_MAGIC) - 1) == 0) {
        bootsel_requested = true;                // handled in the main loop
        return;
    }
    P_LOCK();
    portal_handle_command(buffer, bufsize, millis());
    P_UNLOCK();
}
