#ifndef _LCD_DISPLAY_H_
#define _LCD_DISPLAY_H_

#include "driver/gpio.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel;
    gpio_num_t bl_pin;
    bool initialized;
} lcd_display_t;

bool lcd_init(lcd_display_t *lcd,
              gpio_num_t cs, gpio_num_t dc, gpio_num_t rst,
              gpio_num_t mosi, gpio_num_t sclk, gpio_num_t bl);

void lcd_set_backlight(lcd_display_t *lcd, bool on);
void lcd_fill_color(lcd_display_t *lcd, uint16_t color);
void lcd_draw_rect(lcd_display_t *lcd, int x, int y, int w, int h, uint16_t color);
void lcd_test_pattern(lcd_display_t *lcd);
bool lcd_display_jpeg(lcd_display_t *lcd, const uint8_t *jpeg_data, size_t jpeg_size);

#ifdef __cplusplus
}
#endif

#endif // _LCD_DISPLAY_H_
