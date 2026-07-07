/**
 * @file wake_word_detector.c
 * @brief 唤醒词检测模块 — 基于 ESP-SR AFE + WakeNet
 *
 * 架构：
 *   双任务设计（参考 xiaozhi）：
 *   - audio_feed_task（Core 1）：读取 I2S stereo 数据，feed 给 AFE
 *   - wake_word_wait()（主任务）：fetch_with_delay 阻塞等待唤醒词检测结果
 *
 * AFE 配置：
 *   - 输入格式 "MM"（2 麦克风通道），直接喂 interleaved stereo 数据
 *   - AFE_TYPE_SR + AFE_MODE_HIGH_PERF
 *   - SE（信号增强）、VAD（语音活动检测）、WakeNet 均启用
 *
 * 关键点：
 *   - feed 和 fetch 必须在不同任务中运行，否则 ringbuffer 溢出
 *   - 检测到唤醒词后，feed task 通过 s_wake_detected 标志退出
 *   - 15 秒静默超时在 main.c 的 wake_word_task 中实现
 */

#include "wake_word_detector.h"
#include "audio_codec.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>

#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

static const char *TAG = "WakeWord";

static const esp_afe_sr_iface_t *s_afe = NULL;     // AFE 句柄
static esp_afe_sr_data_t *s_afe_data = NULL;       // AFE 运行时数据
static srmodel_list_t *s_models = NULL;             // 语音模型列表
static audio_codec_t *s_codec = NULL;               // 音频编解码器引用
static volatile bool s_wake_detected = false;       // 唤醒词检测标志

/** 绑定音频编解码器（在 wake_word_init 之前调用） */
void wake_word_set_codec(audio_codec_t *codec) {
    s_codec = codec;
}

/**
 * @brief 初始化唤醒词检测
 *
 * 加载语音模型 → 配置 AFE → 创建 AFE 实例。
 * AFE 输入格式 "MM" = 2 个麦克风通道，直接喂 stereo 数据。
 */
bool wake_word_init(uint32_t sample_rate) {
    ESP_LOGI(TAG, "初始化唤醒词检测...");

    // 加载 SR 模型（WakeNet 唤醒词模型 + AFE 模型）
    s_models = esp_srmodel_init("model");
    if (!s_models) {
        ESP_LOGE(TAG, "加载语音模型失败");
        return false;
    }

    // 查找唤醒词模型
    char *wakenet_model = esp_srmodel_filter(s_models, ESP_WN_PREFIX, NULL);
    if (!wakenet_model) {
        ESP_LOGE(TAG, "未找到唤醒词模型");
        return false;
    }
    ESP_LOGI(TAG, "唤醒词模型: %s", wakenet_model);

    // 配置 AFE：2 通道麦克风，高性能模式
    afe_config_t *afe_config = afe_config_init("MM", s_models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF);
    afe_config->aec_init = false;           // 不使用回声消除（无硬件参考信号）
    afe_config->se_init = true;             // 启用信号增强
    afe_config->vad_init = true;            // 启用语音活动检测
    afe_config->wakenet_init = true;        // 启用唤醒词检测
    afe_config->wakenet_model_name = wakenet_model;
    afe_config->afe_perferred_core = 1;     // AFE 处理在 Core 1
    afe_config->afe_perferred_priority = 1;
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;  // 优先使用 PSRAM

    s_afe = esp_afe_handle_from_config(afe_config);
    if (!s_afe) {
        ESP_LOGE(TAG, "创建 AFE 句柄失败");
        return false;
    }

    s_afe_data = s_afe->create_from_config(afe_config);
    if (!s_afe_data) {
        ESP_LOGE(TAG, "创建 AFE 数据失败");
        return false;
    }

    ESP_LOGI(TAG, "唤醒词检测初始化完成");
    return true;
}

/**
 * @brief 音频输入任务（Core 1）
 *
 * 持续读取 I2S stereo 数据并 feed 给 AFE。
 * 当 s_wake_detected 变为 true 时退出。
 *
 * 为什么用独立任务：
 *   AFE 的 feed 和 fetch 必须在不同上下文中运行，
 *   否则 ringbuffer 会溢出（feed 远超实时速率）。
 */
static void audio_feed_task(void *arg) {
    int feed_chunksize = s_afe->get_feed_chunksize(s_afe_data);
    int chunk_samples = feed_chunksize * 2;  // 2 通道 stereo
    int16_t *buf = heap_caps_malloc(chunk_samples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(chunk_samples * sizeof(int16_t));

    ESP_LOGI(TAG, "Feed task started, chunk=%d", feed_chunksize);
    audio_codec_enable_input(s_codec, true);

    // 循环读取音频并喂给 AFE，直到唤醒词被检测到
    while (!s_wake_detected) {
        if (audio_codec_input(s_codec, buf, chunk_samples)) {
            s_afe->feed(s_afe_data, buf);  // 直接喂 stereo 数据
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    free(buf);
    vTaskDelete(NULL);
}

/**
 * @brief 等待唤醒词检测（阻塞）
 *
 * 启动 feed task，然后在主循环中 fetch_with_delay 等待结果。
 * 检测到唤醒词后返回 true。
 *
 * 注意：此函数会阻塞当前任务，直到唤醒词被检测到。
 *       调用方需要确保 WebSocket 已连接。
 */
bool wake_word_wait(void) {
    if (!s_afe || !s_afe_data) return false;
    if (!s_codec) {
        ESP_LOGE(TAG, "Audio codec not set");
        return false;
    }

    s_wake_detected = false;

    // 启动独立的音频输入任务（Core 1）
    TaskHandle_t feed_task = NULL;
    xTaskCreatePinnedToCore(audio_feed_task, "afe_feed", 4096, NULL, 4, &feed_task, 1);

    ESP_LOGI(TAG, "Listening for wake word...");

    // 主循环：阻塞等待 AFE 处理结果
    while (!s_wake_detected) {
        afe_fetch_result_t *result = s_afe->fetch_with_delay(s_afe_data, portMAX_DELAY);
        if (result && result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "Wake word detected!");
            s_wake_detected = true;
            audio_codec_enable_input(s_codec, false);  // 停止音频输入
            vTaskDelay(pdMS_TO_TICKS(100));  // 等待 feed task 退出
            return true;
        }
    }

    return false;
}

/**
 * @brief 检查是否有语音活动（非阻塞）
 *
 * 用于录音阶段的静默检测：
 *   每 100ms 调用一次，连续 150 次无语音则停止录音。
 */
bool wake_word_is_voice_detected(void) {
    if (!s_afe || !s_afe_data) return false;
    afe_fetch_result_t *result = s_afe->fetch_with_delay(s_afe_data, 0);
    return result && result->vad_state == (vad_state_t)AFE_VAD_SPEECH;
}

/** 检查唤醒词检测是否已初始化 */
bool wake_word_detector_is_ready(void) {
    return (s_afe != NULL && s_afe_data != NULL);
}