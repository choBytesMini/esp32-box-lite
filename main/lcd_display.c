#include "lcd_display.h"
#include "config.h"
#include "esp_log.h"
#include "esp_lcd_ili9341.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tjpgd.h"
#include "tjpgdcnf.h"

static const char *TAG = "LcdDisplay";

// ILI9341 初始化命令 (来自 xiaozhi 官方)
static const ili9341_lcd_init_cmd_t vendor_specific_init[] = {
    {0xC8, (uint8_t []){0xFF, 0x93, 0x42}, 3, 0},
    {0xC0, (uint8_t []){0x0E, 0x0E}, 2, 0},
    {0xC5, (uint8_t []){0xD0}, 1, 0},
    {0xC1, (uint8_t []){0x02}, 1, 0},
    {0xB4, (uint8_t []){0x02}, 1, 0},
    {0xE0, (uint8_t []){0x00, 0x03, 0x08, 0x06, 0x13, 0x09, 0x39, 0x39, 0x48, 0x02, 0x0a, 0x08, 0x17, 0x17, 0x0F}, 15, 0},
    {0xE1, (uint8_t []){0x00, 0x28, 0x29, 0x01, 0x0d, 0x03, 0x3f, 0x33, 0x52, 0x04, 0x0f, 0x0e, 0x37, 0x38, 0x0F}, 15, 0},
    {0xB1, (uint8_t []){0x00, 0x1B}, 2, 0},
    {0x36, (uint8_t []){0x08}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},
    {0xB7, (uint8_t []){0x06}, 1, 0},
    {0x11, (uint8_t []){0}, 0x80, 0},  // Sleep out + delay
    {0x29, (uint8_t []){0}, 0x80, 0},  // Display on + delay
    {0, (uint8_t []){0}, 0xff, 0},     // End marker
};

bool lcd_init(lcd_display_t *lcd,
              gpio_num_t cs, gpio_num_t dc, gpio_num_t rst,
              gpio_num_t mosi, gpio_num_t sclk, gpio_num_t bl) {
    lcd->bl_pin = bl;
    lcd->io_handle = NULL;
    lcd->panel = NULL;

    ESP_LOGI(TAG, "初始化 LCD...");

    // 背光 (反转: LOW=亮)
    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = (1ULL << bl);
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    gpio_config(&bl_cfg);
    gpio_set_level(bl, 0);  // LOW = 亮
    ESP_LOGI(TAG, "背光已打开 (GPIO%d, LOW=亮)", bl);

    // SPI 总线
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = mosi;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = sclk;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 320 * 240 * 2;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &bus_cfg, SPI_DMA_CH_AUTO));
    ESP_LOGI(TAG, "SPI 总线初始化完成 (SPI3_HOST)");

    // LCD IO
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = cs;
    io_config.dc_gpio_num = dc;
    io_config.spi_mode = 0;
    io_config.pclk_hz = 40 * 1000 * 1000;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &lcd->io_handle));
    ESP_LOGI(TAG, "LCD IO 初始化完成 (CS=%d, DC=%d)", cs, dc);

    // ILI9341 驱动 (使用官方 vendor 配置)
    const ili9341_vendor_config_t vendor_config = {
        .init_cmds = &vendor_specific_init[0],
        .init_cmds_size = sizeof(vendor_specific_init) / sizeof(ili9341_lcd_init_cmd_t),
    };

    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = rst;
    panel_config.flags.reset_active_high = 0;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = (void *)&vendor_config;
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(lcd->io_handle, &panel_config, &lcd->panel));
    ESP_LOGI(TAG, "ILI9341 驱动初始化完成 (RST=%d)", rst);

    // 复位并初始化
    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd->panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd->panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd->panel, true));

    lcd->initialized = true;
    ESP_LOGI(TAG, "LCD 初始化完成 (ILI9341, 320x240)");
    return true;
}

void lcd_set_backlight(lcd_display_t *lcd, bool on) {
    if (lcd->bl_pin != GPIO_NUM_NC) {
        gpio_set_level(lcd->bl_pin, on ? 0 : 1);  // 反转: LOW=亮
    }
}

void lcd_fill_color(lcd_display_t *lcd, uint16_t color) {
    if (!lcd->initialized || !lcd->panel) return;

    uint16_t *line = heap_caps_malloc(320 * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line) return;

    for (int i = 0; i < 320; i++) line[i] = color;

    for (int y = 0; y < 240; y++) {
        esp_lcd_panel_draw_bitmap(lcd->panel, 0, y, 320, y + 1, line);
    }

    free(line);
}

void lcd_draw_rect(lcd_display_t *lcd, int x, int y, int w, int h, uint16_t color) {
    if (!lcd->initialized || !lcd->panel) return;

    uint16_t *line = heap_caps_malloc(w * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line) return;

    for (int i = 0; i < w; i++) line[i] = color;

    for (int row = y; row < y + h && row < 240; row++) {
        esp_lcd_panel_draw_bitmap(lcd->panel, x, row, x + w, row + 1, line);
    }

    free(line);
}

void lcd_test_pattern(lcd_display_t *lcd) {
    if (!lcd->initialized) return;

    // 彩色测试图案
    lcd_fill_color(lcd, 0x0000);  // 黑色
    lcd_draw_rect(lcd, 0, 0, 107, 80, 0xF800);    // 红色
    lcd_draw_rect(lcd, 107, 0, 106, 80, 0x07E0);   // 绿色
    lcd_draw_rect(lcd, 213, 0, 107, 80, 0x001F);   // 蓝色
    lcd_draw_rect(lcd, 0, 80, 107, 80, 0xFFE0);    // 黄色
    lcd_draw_rect(lcd, 107, 80, 106, 80, 0xF81F);  // 紫色
    lcd_draw_rect(lcd, 213, 80, 107, 80, 0x07FF);  // 青色
    lcd_draw_rect(lcd, 0, 160, 160, 80, 0xFFFF);   // 白色
    lcd_draw_rect(lcd, 160, 160, 160, 80, 0x8410); // 灰色
}

// JPEG 解码上下文
typedef struct {
    const uint8_t *data;
    size_t size;
    size_t offset;
    lcd_display_t *lcd;
    int x_offset;
    int y_offset;
} jpeg_context_t;

// 全局上下文 (用于回调)
static jpeg_context_t *g_jpeg_ctx = NULL;

// JPEG 输入回调
static size_t jpeg_input_cb(JDEC *jdec, uint8_t *buf, size_t len) {
    jpeg_context_t *ctx = (jpeg_context_t *)jdec->device;
    size_t remaining = ctx->size - ctx->offset;
    if (len > remaining) len = remaining;
    if (buf) {
        memcpy(buf, ctx->data + ctx->offset, len);
    }
    ctx->offset += len;
    return len;
}

// JPEG 输出回调
static int jpeg_output_cb(JDEC *jdec, void *bitmap, JRECT *rect) {
    jpeg_context_t *ctx = (jpeg_context_t *)jdec->device;
    uint16_t *pixels = (uint16_t *)bitmap;

    int x = rect->left + ctx->x_offset;
    int y = rect->top + ctx->y_offset;
    int w = rect->right - rect->left + 1;
    int h = rect->bottom - rect->top + 1;

    // 转换字节序 (TJpgDec 输出小端序，LCD 需要大端序)
    for (int i = 0; i < w * h; i++) {
        uint16_t pixel = pixels[i];
        pixels[i] = ((pixel & 0xFF) << 8) | ((pixel >> 8) & 0xFF);
    }

    for (int row = 0; row < h; row++) {
        int draw_y = y + row;
        if (draw_y >= 0 && draw_y < 240 && x >= 0 && x + w <= 320) {
            esp_lcd_panel_draw_bitmap(ctx->lcd->panel, x, draw_y, x + w, draw_y + 1, pixels + row * w);
        }
    }
    return 1;  // Continue
}

bool lcd_display_jpeg(lcd_display_t *lcd, const uint8_t *jpeg_data, size_t jpeg_size) {
    if (!lcd->initialized || !lcd->panel) {
        ESP_LOGE(TAG, "LCD 未初始化");
        return false;
    }

    ESP_LOGI(TAG, "开始解码 JPEG (%d bytes)...", jpeg_size);

    // 创建上下文
    jpeg_context_t ctx = {
        .data = jpeg_data,
        .size = jpeg_size,
        .offset = 0,
        .lcd = lcd,
        .x_offset = 0,
        .y_offset = 0
    };
    g_jpeg_ctx = &ctx;

    // 分配工作缓冲区 (TJpgDec 需要至少 3100 字节，大图片需要更多)
    void *work_buf = heap_caps_malloc(8192, MALLOC_CAP_DMA);
    if (!work_buf) {
        ESP_LOGE(TAG, "工作缓冲区分配失败");
        return false;
    }
    ESP_LOGI(TAG, "工作缓冲区已分配 (8192 bytes)");

    // 初始化 JPEG 解码器
    JDEC jdec;
    memset(&jdec, 0, sizeof(jdec));
    jdec.device = &ctx;

    JRESULT res = jd_prepare(&jdec, jpeg_input_cb, work_buf, 8192, &ctx);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 准备失败: %d", res);
        free(work_buf);
        return false;
    }

    ESP_LOGI(TAG, "JPEG 图像: %dx%d", jdec.width, jdec.height);

    // 计算居中位置
    ctx.x_offset = (320 - (int)jdec.width) / 2;
    ctx.y_offset = (240 - (int)jdec.height) / 2;
    if (ctx.x_offset < 0) ctx.x_offset = 0;
    if (ctx.y_offset < 0) ctx.y_offset = 0;

    ESP_LOGI(TAG, "显示位置: (%d, %d)", ctx.x_offset, ctx.y_offset);

    // 重置偏移量
    ctx.offset = 0;
    jdec.device = &ctx;

    // 清屏
    lcd_fill_color(lcd, 0x0000);

    // 解码并显示
    ESP_LOGI(TAG, "开始解码...");
    res = jd_decomp(&jdec, jpeg_output_cb, 0);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "JPEG 解码失败: %d", res);
        free(work_buf);
        return false;
    }

    free(work_buf);
    g_jpeg_ctx = NULL;
    ESP_LOGI(TAG, "JPEG 显示完成");
    return true;
}
