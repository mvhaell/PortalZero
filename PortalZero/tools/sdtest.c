// SD card test: talks to the SD card only, without USB and without the portal.
// Retries every second and shows the result.
// Hold SEL while plugging in = flash mode (same as Portal Zero).
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "ff.h"
#include "diskio.h"
#include "hw_config.h"
#include "sd_card.h"
#include "oled.h"

// SEL button on the RP2040-Zero: GP7 to GND
#define BUTTON_SELECT_PIN   7
#define SD_MISO_PIN         12

static FATFS fs;

static void line(int row, const char *s) { oled_text(0, 11 + 8 * row, s, false); }

int main(void) {
    gpio_init(BUTTON_SELECT_PIN); gpio_set_dir(BUTTON_SELECT_PIN, GPIO_IN); gpio_pull_up(BUTTON_SELECT_PIN);
    sleep_ms(20);
    if (!gpio_get(BUTTON_SELECT_PIN)) reset_usb_boot(0, 0);   // SEL held = flash mode

    stdio_init_all();
    oled_init();

    // level of MISO before starting (should be 1 with the pull-up)
    gpio_init(SD_MISO_PIN); gpio_set_dir(SD_MISO_PIN, GPIO_IN); gpio_pull_up(SD_MISO_PIN);
    sleep_ms(5);
    int miso_idle = gpio_get(SD_MISO_PIN);

    for (int attempt = 1; ; attempt++) {
        sd_card_t *sd = sd_get_by_num(0);
        f_unmount(sd->pcName);
        sd->m_Status |= STA_NOINIT;                       // fully restart every time
        FRESULT fr = f_mount(&fs, sd->pcName, 1);

        char buf[24];
        oled_clear();
        oled_text_center(0, "SD card test", false);
        oled_hline(9);
        snprintf(buf, sizeof(buf), "Attempt %d", attempt);
        line(0, buf);
        if (fr == FR_OK) {
            DWORD free_clust;
            FATFS *pfs;
            unsigned mb = 0;
            if (f_getfree(sd->pcName, &free_clust, &pfs) == FR_OK)
                mb = (unsigned)((uint64_t)(pfs->n_fatent - 2) * pfs->csize / 2048);
            line(1, "Result: OK!");
            snprintf(buf, sizeof(buf), "Size: %u MB", mb);
            line(2, buf);
            snprintf(buf, sizeof(buf), "Format: %s", fs.fs_type == FS_EXFAT ? "exFAT" :
                     fs.fs_type == FS_FAT32 ? "FAT32" : "FAT");
            line(3, buf);
        } else {
            snprintf(buf, sizeof(buf), "Result: code %d", (int)fr);
            line(1, buf);
            line(2, fr == FR_NOT_READY ? "Card not responding" :
                    fr == FR_NO_FILESYSTEM ? "Wrong format" : "Read error");
        }
        snprintf(buf, sizeof(buf), "MISO idle: %d", miso_idle);
        line(4, buf);
        oled_hline(54);
        oled_text_center(56, "next try in 1 s", false);
        oled_show();
        sleep_ms(1000);
    }
}
