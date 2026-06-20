#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"

#include "config.h"
#include "audio_codec.h"
#include "lcd_display.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_mp3_dec.h"

static const char *TAG = "main";

static audio_codec_t g_audio_codec;
static lcd_display_t g_lcd;
static i2c_master_bus_handle_t g_i2c_bus = NULL;

extern const uint8_t mp3_start[] asm("_binary_output_mp3_start");
extern const uint8_t mp3_end[]   asm("_binary_output_mp3_end");

static esp_err_t init_i2c(void) {
    ESP_LOGI(TAG, "初始化 I2C...");
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_1;
    bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = 1;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &g_i2c_bus));

    ESP_LOGI(TAG, "扫描 I2C 设备:");
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_device_config_t dev_cfg = {};
        dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_cfg.device_address = addr;
        dev_cfg.scl_speed_hz = 100000;
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(g_i2c_bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t dummy;
            if (i2c_master_receive(dev, &dummy, 1, 100) == ESP_OK) {
                ESP_LOGI(TAG, "  I2C: 0x%02X", addr);
            }
            i2c_master_bus_rm_device(dev);
        }
    }
    ESP_LOGI(TAG, "I2C 初始化完成");
    return ESP_OK;
}

static void play_mp3_task(void *arg) {
    audio_codec_t *codec = (audio_codec_t *)arg;

    ESP_LOGI(TAG, "MP3 文件大小: %d bytes", mp3_end - mp3_start);

    // 注册 MP3 解码器
    esp_audio_dec_register_default();
    ESP_LOGI(TAG, "MP3 解码器已注册");

    // 打开解码器
    esp_audio_dec_cfg_t dec_cfg = { .type = ESP_AUDIO_TYPE_MP3 };
    esp_audio_dec_handle_t decoder = NULL;
    esp_audio_err_t ret = esp_audio_dec_open(&dec_cfg, &decoder);
    if (ret != ESP_AUDIO_ERR_OK || !decoder) {
        ESP_LOGE(TAG, "打开 MP3 解码器失败: %d", ret);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MP3 解码器已打开");

    // 解码缓冲区
    const int out_buf_size = 4096 * 4;
    uint8_t *out_buf = heap_caps_malloc(out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) out_buf = malloc(out_buf_size);

    // 输入数据指针
    uint8_t *mp3_ptr = (uint8_t *)mp3_start;
    uint32_t mp3_remaining = mp3_end - mp3_start;
    bool header_parsed = false;

    ESP_LOGI(TAG, "开始解码播放 MP3...");
    audio_codec_enable_output(codec, true);

    while (mp3_remaining > 0) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = mp3_ptr,
            .len = mp3_remaining,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = out_buf,
            .len = out_buf_size,
        };

        ret = esp_audio_dec_process(decoder, &raw, &frame);
        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGW(TAG, "输出缓冲区不足，需要 %d", frame.needed_size);
            break;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "解码错误: %d, consumed=%d", ret, raw.consumed);
            if (raw.consumed == 0) break;
            mp3_ptr += raw.consumed;
            mp3_remaining -= raw.consumed;
            continue;
        }

        // 第一次成功解码，获取音频信息
        if (!header_parsed && frame.decoded_size > 0) {
            esp_audio_dec_info_t info = {};
            if (esp_audio_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                ESP_LOGI(TAG, "MP3 信息: %dHz, %dbit, %dch, %dbps",
                         info.sample_rate, info.bits_per_sample, info.channel, info.bitrate);
            }
            header_parsed = true;
        }

        // 输出 PCM 数据
        if (frame.decoded_size > 0) {
            audio_codec_output(codec, (int16_t *)frame.buffer, frame.decoded_size / 2);
        }

        mp3_ptr += raw.consumed;
        mp3_remaining -= raw.consumed;
    }

    ESP_LOGI(TAG, "MP3 播放完成");
    esp_audio_dec_close(decoder);
    free(out_buf);

    // MP3播放完成后测试麦克风
    ESP_LOGI(TAG, "--- 测试麦克风 ---");
    audio_codec_enable_input(codec, true);
    for (int i = 0; i < 10; i++) {
        int level = audio_codec_detect_voice(codec, 200);
        ESP_LOGI(TAG, "麦克风电平: %d (阈值>500为有声音)", level);
        if (level > 500) {
            ESP_LOGI(TAG, "✓ 检测到声音!");
        }
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    vTaskDelete(NULL);
}

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-S3-BOX-Lite 智能管家终端");
    ESP_LOGI(TAG, "========================================");

    // 1. I2C
    ESP_LOGI(TAG, "[1] 初始化 I2C...");
    if (init_i2c() != ESP_OK) {
        ESP_LOGE(TAG, "I2C 初始化失败");
        return;
    }

    // 2. LCD
    ESP_LOGI(TAG, "[2] 初始化 LCD...");
    lcd_init(&g_lcd,
             LCD_SPI_CS_PIN, LCD_SPI_DC_PIN, LCD_SPI_RST_PIN,
             LCD_SPI_MOSI_PIN, LCD_SPI_SCLK_PIN, LCD_BACKLIGHT_PIN);
    lcd_test_pattern(&g_lcd);

    // 3. 音频 (ES8156 DAC)
    ESP_LOGI(TAG, "[3] 初始化音频...");
    audio_codec_init(&g_audio_codec, g_i2c_bus,
                     AUDIO_CODEC_PA_PIN,
                     AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                     AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
    audio_codec_enable_output(&g_audio_codec, true);
    audio_codec_set_volume(&g_audio_codec, 80);

    // 4. 播放 MP3
    ESP_LOGI(TAG, "[4] 播放 MP3...");
    xTaskCreatePinnedToCore(play_mp3_task, "mp3", 16384, &g_audio_codec, 5, NULL, 1);

    // 5. 显示 JPEG
    ESP_LOGI(TAG, "[5] 显示 test.jpg...");
    extern const uint8_t test_jpg_start[] asm("_binary_test_jpg_start");
    extern const uint8_t test_jpg_end[] asm("_binary_test_jpg_end");
    lcd_display_jpeg(&g_lcd, test_jpg_start, test_jpg_end - test_jpg_start);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " 初始化完成!");
    ESP_LOGI(TAG, "========================================");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
