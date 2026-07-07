/**
 * @file audio_codec.c
 * @brief 音频编解码器驱动 — ESP32-S3-BOX-Lite
 *
 * 硬件架构：
 *   ES8156 (DAC) — I2C 地址 0x08，负责扬声器输出
 *   ES7243E (ADC) — I2C 地址 0x10，负责麦克风输入
 *   两者共享 I2S 时钟线（MCLK/BCLK/WS），数据线独立（DOUT/DIN）
 *
 * I2S 配置：
 *   TX = STD Philips 模式，MONO，16kHz（只用 dout 引脚）
 *   RX = STD Philips 模式，STEREO，16kHz（只用 din 引脚）
 *   TX/RX 共用 I2S_NUM_0，共享时钟
 *
 * 关键设计决策：
 *   - 不使用 esp_codec_dev 管理 I2S 读写（会导致 DMA 冻结）
 *   - ES7243E 只通过 I2C 配置寄存器，I2S 数据直接用 i2s_channel_read 读取
 *   - 用 mutex 保护 I2S 读写，防止 DMA 并发死锁
 *   - I2S DMA 中断绑定在 Core 1，所有 I2S 操作必须在 Core 1 执行
 */

#include "audio_codec.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <math.h>

#include "es7243e_adc.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"

static const char *TAG = "AudioCodec";

/** I2S 读写互斥锁，防止 TX/RX 并发操作导致 DMA 死锁 */
static SemaphoreHandle_t s_i2s_mutex = NULL;
/** speaker codec 设备句柄（用于 esp_codec_dev_write） */
static esp_codec_dev_handle_t s_spk_handle = NULL;

bool audio_codec_init(audio_codec_t *codec, i2c_master_bus_handle_t i2c_bus,
                      gpio_num_t pa_pin,
                      gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                      gpio_num_t dout, gpio_num_t din) {
    memset(codec, 0, sizeof(audio_codec_t));
    codec->pa_pin = pa_pin;
    codec->output_volume = 50;

    ESP_LOGI(TAG, "初始化音频编解码器...");

    // === 1. PA 功放初始化（GPIO46 控制） ===
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << pa_pin),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(pa_pin, 1);  // 拉高使能功放
    ESP_LOGI(TAG, "功放已打开 (GPIO%d=HIGH)", pa_pin);

    // === 2. I2S 通道创建（TX + RX 共享 I2S_NUM_0） ===
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 12,      // DMA 描述符数量
        .dma_frame_num = 480,    // 每个 DMA 缓冲区的帧数
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &codec->tx_handle, &codec->rx_handle));

    // === 3. TX 配置：STD Philips 模式，MONO，16kHz ===
    // TX 只用 dout 引脚，din 设为 UNUSED
    i2s_std_config_t tx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = mclk, .bclk = bclk, .ws = ws,
            .dout = dout,
            .din = I2S_GPIO_UNUSED,  // TX 不用 din
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    // === 4. RX 配置：STD Philips 模式，STEREO，16kHz ===
    // RX 只用 din 引脚，dout 设为 UNUSED
    i2s_std_config_t rx_cfg = {
        .clk_cfg = {
            .sample_rate_hz = 16000,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = mclk, .bclk = bclk, .ws = ws,
            .dout = I2S_GPIO_UNUSED,  // RX 不用 dout
            .din = din,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    // 初始化并启用 I2S 通道
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->tx_handle, &tx_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(codec->rx_handle, &rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(codec->tx_handle));
    ESP_ERROR_CHECK(i2s_channel_enable(codec->rx_handle));

    // 创建 I2S 读写互斥锁
    s_i2s_mutex = xSemaphoreCreateMutex();
    ESP_LOGI(TAG, "I2S: TX=STD MONO, RX=STD STEREO");

    // === 5. ES8156 DAC 初始化（纯 I2C 配置） ===
    // ES8156 负责扬声器输出，通过 I2C 配置寄存器
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES8156_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!ctrl_if) {
        ESP_LOGE(TAG, "创建 I2C 控制接口失败");
        return false;
    }
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8156_codec_cfg_t es8156_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .pa_pin = pa_pin,
    };
    const audio_codec_if_t *es8156_if = es8156_codec_new(&es8156_cfg);

    // 创建 speaker codec 设备（用于 esp_codec_dev_write 输出音频）
    audio_codec_i2s_cfg_t spk_i2s_cfg = {
        .port = I2S_NUM_0, .rx_handle = NULL, .tx_handle = codec->tx_handle,
    };
    const audio_codec_data_if_t *spk_data_if = audio_codec_new_i2s_data(&spk_i2s_cfg);
    esp_codec_dev_cfg_t spk_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = es8156_if, .data_if = spk_data_if,
    };
    s_spk_handle = esp_codec_dev_new(&spk_dev_cfg);
    if (s_spk_handle) {
        esp_codec_dev_sample_info_t spk_fs = {
            .bits_per_sample = 16, .channel = 1, .channel_mask = 0, .sample_rate = 16000,
        };
        esp_codec_dev_open(s_spk_handle, &spk_fs);
        esp_codec_dev_set_out_vol(s_spk_handle, 100);
        esp_codec_dev_set_out_mute(s_spk_handle, false);
        // esp_codec_dev_open 内部 _i2s_data_set_fmt 会 disable TX 再 reconfig
        // 必须显式重新 enable TX channel
        i2s_channel_enable(codec->tx_handle);
        ESP_LOGI(TAG, "Speaker codec opened (vol=100)");
    }

    // === 6. ES7243E ADC 初始化（纯 I2C 配置） ===
    // ES7243E 负责麦克风输入，只通过 I2C 配置，不走 esp_codec_dev 的 I2S 路径
    // 原因：esp_codec_dev_open 会重新配置 I2S RX DMA，导致系统冻结
    audio_codec_i2c_cfg_t mic_i2c_cfg = {
        .port = I2C_NUM_1,
        .addr = ES7243E_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_bus,
    };
    const audio_codec_ctrl_if_t *mic_ctrl_if = audio_codec_new_i2c_ctrl(&mic_i2c_cfg);
    if (!mic_ctrl_if) {
        ESP_LOGW(TAG, "创建 ES7243E I2C 控制接口失败");
    } else {
        es7243e_codec_cfg_t es7243_cfg = { .ctrl_if = mic_ctrl_if };
        const audio_codec_if_t *es7243_if = es7243e_codec_new(&es7243_cfg);
        if (!es7243_if) {
            ESP_LOGW(TAG, "创建 ES7243E 接口失败");
        } else {
            // es7243e_codec_new 内部已设置 ~30dB 增益
            // 通过 I2C 直接写寄存器提升到 37.5dB
            // get_db_reg(37.5) = 14，寄存器值 = 0x10 | 14 = 0x1E
            uint8_t gain = 0x1E;
            mic_ctrl_if->write_reg(mic_ctrl_if, 0x20, 1, &gain, 1);  // 左声道增益
            mic_ctrl_if->write_reg(mic_ctrl_if, 0x21, 1, &gain, 1);  // 右声道增益
            ESP_LOGI(TAG, "ES7243E 初始化成功 (+37.5dB)");
        }
    }

    ESP_LOGI(TAG, "音频编解码器初始化完成");
    return true;
}

/** 启用/禁用音频输出（设置标志位，不操作硬件） */
void audio_codec_enable_output(audio_codec_t *codec, bool enable) {
    codec->output_enabled = enable;
}

/** 启用/禁用音频输入（设置标志位，不操作硬件） */
void audio_codec_enable_input(audio_codec_t *codec, bool enable) {
    codec->input_enabled = enable;
}

/** 设置输出音量（0-100） */
void audio_codec_set_volume(audio_codec_t *codec, int volume) {
    codec->output_volume = volume;
    ESP_LOGI(TAG, "音量设置: %d%%", volume);
}

/**
 * @brief 写音频数据到 I2S TX（扬声器输出）
 *
 * 使用 mutex 保护，防止与 i2s_channel_read 并发导致 DMA 死锁。
 * 注意：I2S DMA 中断绑定在 Core 1，此函数应在 Core 1 调用。
 */
bool audio_codec_output(audio_codec_t *codec, const int16_t *data, int samples) {
    if (!codec->output_enabled || !codec->tx_handle) return false;
    size_t bytes_written = 0;
    int ret = i2s_channel_write(codec->tx_handle, data, samples * sizeof(int16_t), &bytes_written, 500);
    if (ret != ESP_OK) {
        static int err_count = 0;
        if (err_count < 3) {
            ESP_LOGW(TAG, "i2s_channel_write failed: %d, written=%d", ret, (int)bytes_written);
            err_count++;
        }
    }
    return (ret == ESP_OK && bytes_written > 0);
}

/**
 * @brief 从 I2S RX 读取音频数据（麦克风输入）
 *
 * 使用 mutex 保护，防止与 i2s_channel_write 并发导致 DMA 死锁。
 * 返回 stereo 交织数据 [L0,R0,L1,R1,...]，调用方需要提取左声道。
 * 注意：I2S DMA 中断绑定在 Core 1，此函数应在 Core 1 调用。
 */
bool audio_codec_input(audio_codec_t *codec, int16_t *data, int samples) {
    if (!codec->input_enabled || !codec->rx_handle) return false;
    if (s_i2s_mutex && xSemaphoreTake(s_i2s_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
    size_t bytes_read = 0;
    int ret = i2s_channel_read(codec->rx_handle, data, samples * sizeof(int16_t), &bytes_read, 100);
    if (s_i2s_mutex) xSemaphoreGive(s_i2s_mutex);
    return (ret == ESP_OK && bytes_read > 0);
}

/**
 * @brief 检测指定时长内的声音 RMS 电平
 *
 * 临时启用输入，读取音频数据，计算 RMS 后恢复原状态。
 * 用于静默检测和音频质量测试。
 */
int audio_codec_detect_voice(audio_codec_t *codec, int duration_ms) {
    if (!codec->rx_handle) return -1;
    int samples = 16000 * duration_ms / 1000;
    int16_t *buf = heap_caps_malloc(samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(samples * sizeof(int16_t));
    if (!buf) return -1;

    bool was_enabled = codec->input_enabled;
    codec->input_enabled = true;
    audio_codec_input(codec, buf, samples);
    codec->input_enabled = was_enabled;

    int64_t sum_sq = 0;
    for (int i = 0; i < samples; i++) sum_sq += (int32_t)buf[i] * buf[i];
    free(buf);
    return (int)sqrtf((float)sum_sq / samples);
}