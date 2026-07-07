/**
 * @file main.c
 * @brief ESP32-S3-BOX-Lite 智能家居终端主程序
 *
 * 功能模块：
 *   1. I2C + LCD 初始化
 *   2. 音频编解码器初始化（ES8156 + ES7243E）
 *   3. WiFi + MQTT + WebSocket 连接
 *   4. 唤醒词检测（"你好小智"）
 *   5. 流式录音 + Opus 编码 + WebSocket 上传
 *   6. 服务器下发音频播放
 *   7. ADC 按键控制（音量/切歌）
 *   8. LCD 聊天界面显示
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#include "config.h"
#include "audio_codec.h"
#include "lcd_display.h"
#include "codec_benchmark.h"
#include "wifi_manager.h"
#include "app_mqtt_client.h"
#include "audio_recorder.h"
#include "audio_player.h"
#include "http_uploader.h"
#include "wake_word_detector.h"
#include "esp_audio_dec.h"
#include "esp_audio_dec_default.h"
#include "esp_mp3_dec.h"
#include "opus_encoder.h"
#include "opus_decoder.h"
#include "websocket_uploader.h"
#include "audio_stream_protocol.h"

static const char *TAG = "main";

static audio_codec_t g_audio_codec;
static lcd_display_t g_lcd;
static i2c_master_bus_handle_t g_i2c_bus = NULL;
static adc_oneshot_unit_handle_t g_adc_handle = NULL;

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

static esp_err_t init_adc_buttons(void) {
    ESP_LOGI(TAG, "初始化 ADC 按键...");
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = BUTTON_ADC_UNIT,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &g_adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(g_adc_handle, BUTTON_ADC_CHANNEL, &chan_cfg));
    ESP_LOGI(TAG, "ADC 按键初始化完成 (GPIO%d)", BUTTON_ADC_GPIO);
    return ESP_OK;
}

typedef enum {
    BTN_NONE = 0,
    BTN_PREV,
    BTN_ENTER,
    BTN_NEXT,
} button_id_t;

static button_id_t read_button(void) {
    if (!g_adc_handle) return BTN_NONE;
    int raw = 0;
    if (adc_oneshot_read(g_adc_handle, BUTTON_ADC_CHANNEL, &raw) != ESP_OK) {
        return BTN_NONE;
    }
    int mv = raw * 3300 / 4095;
    if (mv >= BUTTON_PREV_MV_MIN && mv <= BUTTON_PREV_MV_MAX) return BTN_PREV;
    if (mv >= BUTTON_ENTER_MV_MIN && mv <= BUTTON_ENTER_MV_MAX) return BTN_ENTER;
    if (mv >= BUTTON_NEXT_MV_MIN && mv <= BUTTON_NEXT_MV_MAX) return BTN_NEXT;
    return BTN_NONE;
}

// ==================== MQTT 回调 ====================

static void on_mqtt_reply(const char *user, const char *text, const char *tts_url) {
    ESP_LOGI(TAG, "回复 %s: %.50s...", user, text);

    // 显示用户消息
    lcd_chat_add_message(&g_lcd, MSG_TYPE_USER, text);

    // 播放 TTS
    if (tts_url && strlen(tts_url) > 0) {
        lcd_chat_set_state(&g_lcd, CHAT_STATE_SPEAKING, "Speaking...");
        audio_player_play_from_url(tts_url);
    }

    lcd_chat_set_state(&g_lcd, CHAT_STATE_IDLE, "Ready");
}

static void on_mqtt_identity(const char *user, float confidence) {
    lcd_show_identity(&g_lcd, user, confidence);
}

static void on_mqtt_alert(void) {
    lcd_show_alert(&g_lcd, "Smoke Alert!");
    audio_player_stop();
}

static void on_mqtt_music_status(const char *state, const char *track, const char *artist) {
    if (strcmp(state, "playing") == 0)
        lcd_show_now_playing(&g_lcd, track, artist, "Playing");
    else if (strcmp(state, "paused") == 0)
        lcd_show_now_playing(&g_lcd, track, artist, "Paused");
    else if (strcmp(state, "stopped") == 0)
        lcd_show_now_playing(&g_lcd, "", "", "Stopped");
    else if (strcmp(state, "not_found") == 0)
        lcd_show_music_error(&g_lcd, "Track not found");
}

static void on_mqtt_skill(const char *user, const char *skill_name) {
    ESP_LOGI(TAG, "Skill switch: %s -> %s", user, skill_name);
    lcd_show_skill(&g_lcd, user, skill_name);
}

// ==================== WebSocket 回调 ====================

static void on_ws_text(const char *text, int len, void *user_ctx) {
    ESP_LOGI(TAG, "WS text: %.*s", len, text);

    cJSON *json = cJSON_ParseWithLength(text, len);
    if (!json) {
        ESP_LOGW(TAG, "WS text is not JSON, ignoring");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(json, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(json);
        return;
    }

    const char *type_str = type->valuestring;

    if (strcmp(type_str, "hello") == 0) {
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Connected to server");
    } else if (strcmp(type_str, "stt") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(json, "text");
        if (cJSON_IsString(text_item)) {
            lcd_chat_add_message(&g_lcd, MSG_TYPE_USER, text_item->valuestring);
        }
        lcd_chat_set_state(&g_lcd, CHAT_STATE_THINKING, "Thinking...");
    } else if (strcmp(type_str, "tts") == 0) {
        cJSON *text_item = cJSON_GetObjectItem(json, "text");
        if (cJSON_IsString(text_item)) {
            lcd_chat_add_message(&g_lcd, MSG_TYPE_AI, text_item->valuestring);
        }
        lcd_chat_set_state(&g_lcd, CHAT_STATE_SPEAKING, "Speaking...");
    } else if (strcmp(type_str, "audio") == 0) {
        lcd_chat_set_state(&g_lcd, CHAT_STATE_SPEAKING, "Speaking...");
    } else {
        ESP_LOGW(TAG, "Unknown WS type: %s", type_str);
    }

    cJSON_Delete(json);
}

static void on_ws_binary(const uint8_t *data, int len, void *user_ctx) {
    if (len < 4) return;

    uint8_t frame_type = data[0];
    uint16_t payload_size = ((uint16_t)data[2] << 8) | data[3];
    if (payload_size + 4 > len) return;

    ESP_LOGI(TAG, "on_ws_binary: type=0x%02x payload=%d len=%d", frame_type, payload_size, len);

    if (frame_type == AUDIO_STREAM_TYPE_OPUS) {
        // Opus 编码帧 → 解码为 PCM → 入队播放
        static int16_t pcm_buf[AUDIO_STREAM_FRAME_SAMPLES];
        int decoded = app_opus_decoder_decode(data + 4, payload_size,
                                              pcm_buf, AUDIO_STREAM_FRAME_SAMPLES);
        if (decoded > 0) {
            audio_player_stream_queue(pcm_buf, decoded);
        }
    } else {
        // 服务器 send_mp3_to_devices 发送的是 PCM 原始音频（type=0x01）
        // 直接入队播放
        const int16_t *pcm_data = (const int16_t *)(data + 4);
        int pcm_samples = payload_size / sizeof(int16_t);
        if (pcm_samples > 0) {
            audio_player_stream_queue(pcm_data, pcm_samples);
        }
    }
}

// ==================== 音频流回调 ====================

/** 最后一次检测到语音的时间戳（用于静默超时） */
static volatile int64_t s_last_voice_time_us = 0;
/** 语音检测 RMS 阈值（原始数据，环境噪声 80-160） */
#define VOICE_RMS_THRESHOLD  200

/** 软件增益倍数（放大麦克风信号） */
#define SOFTWARE_GAIN  3

/**
 * @brief 音频帧回调 — stereo→mono 转换 + Opus 编码 + WebSocket 上传
 *
 * 由 stream_task 调用，传递 stereo 交织数据 [L0,R0,L1,R1,...]
 * 处理流程：
 *   1. 提取左声道为 mono
 *   2. 计算 RMS 并更新语音活动时间戳
 *   3. Opus 编码（16kHz, mono, 60ms）
 *   4. 构建 BP3 帧 [type:1B][reserved:1B][size:2B BE][opus_data]
 *   5. 通过 WebSocket 发送
 */
static void on_audio_frame(const int16_t *pcm, int samples, void *user_ctx) {
    static uint8_t encode_buf[256];
    static uint8_t frame_buf[4 + 256];
    static int16_t mono_buf[AUDIO_STREAM_FRAME_SAMPLES];
    static int frame_count = 0;

    if (!ws_uploader_is_connected()) {
        if (frame_count == 0) ESP_LOGW(TAG, "on_audio_frame: WS not connected");
        return;
    }

    // pcm 是 stereo 交织数据 [L0,R0,L1,R1,...]，samples = stereo 样本数
    // 提取左声道为 mono
    int mono_samples = samples / 2;
    if (mono_samples > AUDIO_STREAM_FRAME_SAMPLES) mono_samples = AUDIO_STREAM_FRAME_SAMPLES;
    for (int i = 0; i < mono_samples; i++) {
        mono_buf[i] = pcm[i * 2];  // 取偶数索引 = 左声道
    }

    // 用原始数据计算 RMS（不受软件增益影响）
    int64_t sum_sq = 0;
    for (int i = 0; i < mono_samples; i++) {
        sum_sq += (int32_t)mono_buf[i] * mono_buf[i];
    }
    int rms = (int)sqrtf((float)sum_sq / mono_samples);
    if (rms > VOICE_RMS_THRESHOLD) {
        s_last_voice_time_us = esp_timer_get_time();
    }

    // 应用软件增益（仅用于 Opus 编码）
    for (int i = 0; i < mono_samples; i++) {
        int32_t amplified = (int32_t)mono_buf[i] * SOFTWARE_GAIN;
        if (amplified > 32767) amplified = 32767;
        if (amplified < -32768) amplified = -32768;
        mono_buf[i] = (int16_t)amplified;
    }

    // Opus 编码
    int encoded = app_opus_encoder_encode(mono_buf, mono_samples, encode_buf, sizeof(encode_buf));
    if (encoded <= 0) {
        ESP_LOGW(TAG, "Opus encode failed: %d", encoded);
        return;
    }

    // 构建 BP3 帧并发送
    bp3_write_header(frame_buf, AUDIO_STREAM_TYPE_OPUS, (uint16_t)encoded);
    memcpy(frame_buf + 4, encode_buf, encoded);

    bool sent = ws_uploader_send_binary(frame_buf, 4 + encoded);
    if (++frame_count <= 5 || frame_count % 50 == 0) {
        ESP_LOGI(TAG, "Audio frame #%d: mono=%d enc=%d sent=%d rms=%d", frame_count, mono_samples, encoded, sent, rms);
    }
}

// ==================== 唤醒词检测 + 流式录音 + WebSocket 上传 ====================

/**
 * @brief 唤醒词检测主任务
 *
 * 完整流程：
 *   1. 等待 WebSocket 连接
 *   2. 调用 wake_word_wait() 阻塞等待唤醒词（"你好小智"）
 *   3. 检测到后启动流式录音（stream_task）
 *   4. 持续监控语音活动，15 秒无语音自动停止
 *   5. 发送 stt_end 信号，等待服务器回复
 *   6. 回到步骤 1，等待下次唤醒词
 *
 * 注意：stream_task 在 Core 1 运行（I2S DMA 中断绑定在 Core 1）
 */
static void wake_word_task(void *arg) {
    bool voice_active = false;
    bool wake_word_ready = wake_word_detector_is_ready();

    if (!wake_word_ready) {
        ESP_LOGW(TAG, "Wake word not available, task idle");
        lcd_chat_set_state(&g_lcd, CHAT_STATE_IDLE, "Wake word N/A");
        while (true) {
            vTaskDelay(pdMS_TO_TICKS(10000));
        }
    }

    while (true) {
        lcd_chat_set_state(&g_lcd, CHAT_STATE_IDLE, "Waiting for wake word...");

        // 等待 WebSocket 连接
        if (!ws_uploader_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // 阻塞等待唤醒词检测
        if (!wake_word_wait()) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // 唤醒词检测到，开始录音
        ESP_LOGI(TAG, "Wake word detected, starting stream...");
        lcd_chat_set_state(&g_lcd, CHAT_STATE_LISTENING, "Listening...");
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Wake word detected!");
        voice_active = true;

        // 停止音频播放，避免 TX/RX DMA 并发死锁
        audio_player_stop();

        // 启动流式录音（Core 1），音频帧通过 on_audio_frame 回调上传
        s_last_voice_time_us = esp_timer_get_time();  // 初始化语音时间戳
        audio_recorder_start_stream(on_audio_frame, NULL, AUDIO_STREAM_FRAME_SAMPLES);

        // 监控语音活动，15 秒无语音自动停止
        // 使用 on_audio_frame 中的 RMS 检测，不依赖 AFE VAD
        while (voice_active) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            int64_t elapsed_ms = (esp_timer_get_time() - s_last_voice_time_us) / 1000;
            if (elapsed_ms > 15000) {  // 15 秒无语音
                ESP_LOGI(TAG, "15s silence detected, stopping stream");
                voice_active = false;
            }
        }

        // 停止录音，发送结束信号
        ESP_LOGI(TAG, "Stopping stream...");
        audio_recorder_stop_stream();
        ESP_LOGI(TAG, "Stream stopped, sending stt_end...");
        lcd_chat_set_state(&g_lcd, CHAT_STATE_THINKING, "Thinking...");
        vTaskDelay(pdMS_TO_TICKS(100));  // 等待 I2S DMA 空闲

        // 启动播放任务（录音已停止，不会 DMA 冲突）
        audio_player_stream_start();

        ws_uploader_send_text("{\"type\":\"stt_end\"}");
        ESP_LOGI(TAG, "stt_end sent, waiting for reply...");
    }
}

// ==================== 按键任务 ====================

static void button_task(void *arg) {
    button_id_t last_btn = BTN_NONE;
    int volume = VOLUME_DEFAULT;

    ESP_LOGI(TAG, "Button task started (volume=%d%%)", volume);

    while (true) {
        button_id_t btn = read_button();

        if (btn != BTN_NONE && btn != last_btn) {
            switch (btn) {
                case BTN_PREV:
                    volume -= VOLUME_STEP;
                    if (volume < VOLUME_MIN) volume = VOLUME_MIN;
                    audio_codec_set_volume(&g_audio_codec, volume);
                    ESP_LOGI(TAG, "[VOL-] %d%%", volume);
                    break;

                case BTN_NEXT:
                    volume += VOLUME_STEP;
                    if (volume > VOLUME_MAX) volume = VOLUME_MAX;
                    audio_codec_set_volume(&g_audio_codec, volume);
                    ESP_LOGI(TAG, "[VOL+] %d%%", volume);
                    break;

                case BTN_ENTER:
                    ESP_LOGI(TAG, "[MID] Starting Codec Benchmark...");
                    lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Running benchmark...");
                    benchmark_result_t result;
                    codec_benchmark_run(&g_audio_codec, &result);
                    lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Benchmark complete!");
                    break;

                default:
                    break;
            }
        }
        last_btn = btn;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ==================== 入口 ====================

void app_main(void) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " ESP32-S3-BOX-Lite Smart Home Terminal");
    ESP_LOGI(TAG, "========================================");

    // 1. I2C
    ESP_LOGI(TAG, "[1] Init I2C...");
    if (init_i2c() != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed");
        return;
    }

    // 2. LCD (Chat UI)
    ESP_LOGI(TAG, "[2] Init LCD Chat UI...");
    lcd_init(&g_lcd, LCD_SPI_CS_PIN, LCD_SPI_DC_PIN, LCD_SPI_RST_PIN,
             LCD_SPI_MOSI_PIN, LCD_SPI_SCLK_PIN, LCD_BACKLIGHT_PIN);
    lcd_start_refresh_task(&g_lcd);
    lcd_chat_set_state(&g_lcd, CHAT_STATE_IDLE, "Starting up...");

    // 3. Audio codec
    ESP_LOGI(TAG, "[3] Init audio...");
    audio_codec_init(&g_audio_codec, g_i2c_bus,
                     AUDIO_CODEC_PA_PIN,
                     AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                     AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
    audio_codec_enable_output(&g_audio_codec, true);
    audio_codec_enable_input(&g_audio_codec, true);
    audio_codec_set_volume(&g_audio_codec, VOLUME_DEFAULT);

    audio_recorder_set_codec(&g_audio_codec);
    audio_player_set_codec(&g_audio_codec);
    audio_player_stream_start();

    // 4. Wi-Fi
    ESP_LOGI(TAG, "[4] Connect Wi-Fi...");
    if (!wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS)) {
        lcd_set_wifi_status(&g_lcd, false);
        lcd_show_error(&g_lcd, "WiFi connection failed!");
    } else {
        lcd_set_wifi_status(&g_lcd, true);
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "WiFi connected");
    }

    // 5. MQTT
    ESP_LOGI(TAG, "[5] Connect MQTT...");
    if (!mqtt_client_init(MQTT_HOST, MQTT_PORT)) {
        lcd_set_mqtt_status(&g_lcd, false);
        lcd_show_error(&g_lcd, "MQTT connection failed!");
    } else {
        lcd_set_mqtt_status(&g_lcd, true);
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "MQTT connected");
    }
    mqtt_client_on_reply(on_mqtt_reply);
    mqtt_client_on_identity(on_mqtt_identity);
    mqtt_client_on_alert(on_mqtt_alert);
    mqtt_client_on_music_status(on_mqtt_music_status);
    mqtt_client_on_skill(on_mqtt_skill);

    // 6. Opus encoder + decoder
    ESP_LOGI(TAG, "[6] Init Opus codec...");
    if (!app_opus_encoder_init()) {
        lcd_show_error(&g_lcd, "Opus encoder init failed!");
    } else {
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Opus encoder ready");
    }
    if (!app_opus_decoder_init()) {
        lcd_show_error(&g_lcd, "Opus decoder init failed!");
    } else {
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "Opus decoder ready");
    }

    // 7. WebSocket
    ESP_LOGI(TAG, "[7] Connect WebSocket...");
    ws_uploader_set_on_text(on_ws_text, NULL);
    ws_uploader_set_on_binary(on_ws_binary, NULL);
    if (ws_uploader_connect(WS_URL)) {
        char hello[256];
        snprintf(hello, sizeof(hello),
            "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
            "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}}",
            AUDIO_STREAM_SAMPLE_RATE, AUDIO_STREAM_CHANNELS, AUDIO_STREAM_FRAME_DURATION_MS);
        ws_uploader_send_text(hello);
        lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "WebSocket connected");
    } else {
        lcd_show_error(&g_lcd, "WebSocket connection failed!");
    }

    // 8. Wake word detection
    ESP_LOGI(TAG, "[8] Init wake word...");
    wake_word_init(AUDIO_INPUT_SAMPLE_RATE);
    wake_word_set_codec(&g_audio_codec);

    // 9. ADC buttons
    ESP_LOGI(TAG, "[9] Init buttons...");
    if (init_adc_buttons() == ESP_OK) {
        xTaskCreatePinnedToCore(button_task, "btn", 4096, NULL, 6, NULL, 0);
    }

    mqtt_client_publish_status("online");
    lcd_show_ready(&g_lcd);

    // Start wake word task
    ESP_LOGI(TAG, "[10] Start wake word task...");
    xTaskCreatePinnedToCore(wake_word_task, "wake_word",
        TASK_WAKE_WORD_STACK_SIZE, NULL, TASK_WAKE_WORD_PRIORITY,
        NULL, TASK_WAKE_WORD_CORE);

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Init complete! Volume=%d%%", VOLUME_DEFAULT);
    ESP_LOGI(TAG, "========================================");

    // Main loop
    while (true) {
        // Update connection status on LCD
        bool wifi_ok = wifi_manager_is_connected();
        static bool last_wifi = false;
        if (wifi_ok != last_wifi) {
            lcd_set_wifi_status(&g_lcd, wifi_ok);
            if (wifi_ok) {
                lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "WiFi reconnected");
            }
            last_wifi = wifi_ok;
        }

        if (!wifi_ok) {
            wifi_manager_connect(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS);
        }

        if (!ws_uploader_is_connected()) {
            ESP_LOGW(TAG, "WebSocket disconnected, reconnecting...");
            if (ws_uploader_connect(WS_URL)) {
                char hello[256];
                snprintf(hello, sizeof(hello),
                    "{\"type\":\"hello\",\"version\":3,\"transport\":\"websocket\","
                    "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,\"channels\":%d,\"frame_duration\":%d}}",
                    AUDIO_STREAM_SAMPLE_RATE, AUDIO_STREAM_CHANNELS, AUDIO_STREAM_FRAME_DURATION_MS);
                ws_uploader_send_text(hello);
                lcd_chat_add_message(&g_lcd, MSG_TYPE_SYSTEM, "WebSocket reconnected");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
