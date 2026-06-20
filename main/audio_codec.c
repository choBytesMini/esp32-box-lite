#include "audio_codec.h"
#include "config.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
#include "es7243e_adc.h"

static const char *TAG = "AudioCodec";

static i2s_chan_handle_t s_tx_handle = NULL;
static i2s_chan_handle_t s_rx_handle = NULL;
static esp_codec_dev_handle_t s_spk_handle = NULL;  // 扬声器
static esp_codec_dev_handle_t s_mic_handle = NULL;   // 麦克风

bool audio_codec_init(audio_codec_t *codec, i2c_master_bus_handle_t i2c_bus,
                      gpio_num_t pa_pin,
                      gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                      gpio_num_t dout, gpio_num_t din) {
    memset(codec, 0, sizeof(audio_codec_t));
    codec->pa_pin = pa_pin;
    codec->output_volume = 80;

    ESP_LOGI(TAG, "初始化音频编解码器...");
    ESP_LOGI(TAG, "I2S: MCLK=%d BCLK=%d WS=%d DOUT=%d DIN=%d",
             mclk, bclk, ws, dout, din);

    // 配置功放引脚
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << pa_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(pa_pin, 1);  // HIGH = 功放开 (官方BSP: pa_reverted=false)
    ESP_LOGI(TAG, "功放已打开 (GPIO%d=HIGH)", pa_pin);

    // 创建 I2S 通道
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_handle));

    // 创建 I2C 控制接口 (ES8156 DAC, 8位地址=0x10)
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8156_CODEC_DEFAULT_ADDR,  // 0x10 (8位格式)
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "创建 I2C 控制接口失败");
        return false;
    }

    // 创建 I2S 数据接口
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) {
        ESP_LOGE(TAG, "创建 I2S 数据接口失败");
        return false;
    }

    // 创建 GPIO 接口
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // 创建 ES8156 DAC 接口
    es8156_codec_cfg_t es8156_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = pa_pin,
        .pa_reverted = false,  // 官方BSP: HIGH = 功放开
    };
    const audio_codec_if_t *es8156_if = es8156_codec_new(&es8156_cfg);
    if (!es8156_if) {
        ESP_LOGE(TAG, "创建 ES8156 接口失败");
        return false;
    }

    // 创建扬声器 codec 设备
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8156_if,
        .data_if = data_if,
    };
    s_spk_handle = esp_codec_dev_new(&dev_cfg);
    if (!s_spk_handle) {
        ESP_LOGE(TAG, "创建扬声器 codec 设备失败");
        return false;
    }

    // 配置采样参数
    esp_codec_dev_sample_info_t sample_cfg = {
        .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
        .channel = 2,
        .channel_mask = 0x03,
        .sample_rate = 44100,
    };
    esp_err_t ret = esp_codec_dev_open(s_spk_handle, &sample_cfg);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "打开扬声器 codec 设备失败: %d", ret);
        return false;
    }

    // 设置音量
    esp_codec_dev_set_out_vol(s_spk_handle, 80);
    esp_codec_dev_set_out_mute(s_spk_handle, false);

    // 创建 ES7243E ADC 接口 (麦克风)
    audio_codec_i2c_cfg_t mic_i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES7243E_CODEC_DEFAULT_ADDR,  // 0x20 (8位格式)
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl_if = audio_codec_new_i2c_ctrl(&mic_i2c_cfg);
    if (!mic_ctrl_if) {
        ESP_LOGW(TAG, "创建 ES7243E I2C 控制接口失败 (麦克风可能不可用)");
    } else {
        es7243e_codec_cfg_t es7243_cfg = {
            .ctrl_if = mic_ctrl_if,
        };
        const audio_codec_if_t *es7243_if = es7243e_codec_new(&es7243_cfg);
        if (!es7243_if) {
            ESP_LOGW(TAG, "创建 ES7243E 接口失败");
        } else {
            esp_codec_dev_cfg_t mic_dev_cfg = {
                .dev_type = ESP_CODEC_DEV_TYPE_IN,
                .codec_if = es7243_if,
                .data_if = data_if,
            };
            s_mic_handle = esp_codec_dev_new(&mic_dev_cfg);
            if (!s_mic_handle) {
                ESP_LOGW(TAG, "创建麦克风 codec 设备失败");
            } else {
                esp_codec_dev_sample_info_t mic_sample_cfg = {
                    .bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
                    .channel = 2,
                    .channel_mask = 0x03,
                    .sample_rate = 16000,
                };
                ret = esp_codec_dev_open(s_mic_handle, &mic_sample_cfg);
                if (ret != ESP_CODEC_DEV_OK) {
                    ESP_LOGW(TAG, "打开麦克风 codec 设备失败: %d", ret);
                } else {
                    ESP_LOGI(TAG, "ES7243E 麦克风初始化成功");
                }
            }
        }
    }

    // 调试: 读取 ES8156 关键寄存器
    int reg_val = 0;
    for (int reg = 0x00; reg <= 0x25; reg++) {
        esp_codec_dev_read_reg(s_spk_handle, reg, &reg_val);
        ESP_LOGI(TAG, "ES8156[%02X] = 0x%02X", reg, reg_val);
    }

    codec->tx_handle = s_tx_handle;
    codec->rx_handle = s_rx_handle;
    ESP_LOGI(TAG, "音频编解码器初始化完成 (ES8156 DAC + ES7243E ADC)");
    return true;
}

void audio_codec_enable_output(audio_codec_t *codec, bool enable) {
    codec->output_enabled = enable;
    if (enable && s_spk_handle) {
        esp_codec_dev_set_out_vol(s_spk_handle, codec->output_volume);
    }
}

void audio_codec_enable_input(audio_codec_t *codec, bool enable) {
    codec->input_enabled = enable;
}

void audio_codec_set_volume(audio_codec_t *codec, int volume) {
    codec->output_volume = volume;
    if (s_spk_handle) {
        esp_codec_dev_set_out_vol(s_spk_handle, volume);
        ESP_LOGI(TAG, "音量设置: %d%%", volume);
    }
}

bool audio_codec_output(audio_codec_t *codec, const int16_t *data, int samples) {
    if (!codec->output_enabled || !s_spk_handle) {
        ESP_LOGW(TAG, "输出未启用: enabled=%d, handle=%p", codec->output_enabled, s_spk_handle);
        return false;
    }
    int ret = esp_codec_dev_write(s_spk_handle, (void *)data, samples * sizeof(int16_t));
    if (ret != 0) {
        ESP_LOGE(TAG, "esp_codec_dev_write 失败: %d", ret);
    }
    return true;
}

bool audio_codec_input(audio_codec_t *codec, int16_t *data, int samples) {
    if (!codec->input_enabled || !s_mic_handle) return false;
    int ret = esp_codec_dev_read(s_mic_handle, data, samples * sizeof(int16_t));
    return (ret == 0);
}

int audio_codec_detect_voice(audio_codec_t *codec, int duration_ms) {
    if (!s_mic_handle) return -1;

    const int sr = 16000;
    int samples = sr * duration_ms / 1000;
    int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(samples * sizeof(int16_t));
    if (!buf) return -1;

    // 录制音频
    esp_codec_dev_read(s_mic_handle, buf, samples * sizeof(int16_t));

    // 计算RMS电平
    int64_t sum_sq = 0;
    for (int i = 0; i < samples; i++) {
        sum_sq += (int32_t)buf[i] * buf[i];
    }
    int rms = (int)sqrtf((float)sum_sq / samples);

    free(buf);
    return rms;
}
