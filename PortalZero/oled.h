// Minimal driver for a 0.96" SSD1306 OLED (128x64) over I2C
#ifndef OLED_H
#define OLED_H

#include <stdbool.h>

// RP2040-Zero wiring: SDA -> GP28, SCL -> GP29 (hardware I2C0), VCC -> 3V3, GND -> GND
#define OLED_I2C      i2c0
#define OLED_SDA_PIN  28
#define OLED_SCL_PIN  29
#define OLED_ADDR     0x3C   // some displays use 0x3D
#define OLED_WIDTH    128
#define OLED_HEIGHT   64

void oled_init(void);
void oled_clear(void);                       // clear the frame buffer only
void oled_show(void);                        // send the frame buffer to the display
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_hline(int y);
void oled_text(int x, int y, const char *s, bool invert);
void oled_text_center(int y, const char *s, bool invert);

#endif
