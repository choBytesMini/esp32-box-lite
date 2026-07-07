/**
 * @file audio_recorder.c
 * @brief 音频录制模块 — 缓冲录音 + 流式录音
 *
 * 两种模式：
 *   1. 缓冲录音（record_task）：一次性录制固定时长的音频到缓冲区
 *   2. 流式录音（stream_task）：持续读取音频帧并通过回调上传
 *
 * 关键设计：
 *   - I2S RX 是 stereo，流式录音需要读取 2x 样本数
 *   - 回调函数负责 stereo→mono 转换和 Opus 编码
 *   - 所有 I2S 操作在 Core 1 执行（I2S DMA 中断绑定在 Core 1）
 */

#include "audio_recorder.h"
#include "config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "AudioRec";

static audio_codec_t *s_codec = NULL;

// === 缓冲录音状态 ===
static audio_buffer_t *s_buf = NULL;
static volatile bool s_recording = false;
static TaskHandle_t s_task_handle = NULL;

// === 流式录音状态 ===
static audio_recorder_frame_cb_t s_frame_cb = NULL;
static void *s_frame_ctx = NULL;
static volatile bool s_streaming = false;
static TaskHandle_t s_stream_task_handle = NULL;
static int s_frame_samples = 0;

/** 绑定音频编解码器 */
void audio_recorder_set_codec(audio_codec_t *codec) {
    s_codec = codec;
}

/**
 * @brief 缓冲录音任务
 *
 * 持续读取 I2S 数据直到缓冲区满或 s_recording 变为 false。
 * 录制完成后自动删除任务。
 */
static void record_task(void *arg) {
    size_t total = 0;
    const size_t chunk_samples = 1024;
    int16_t chunk[chunk_samples];

    audio_codec_enable_input(s_codec, true);

    while (s_recording && total < AUDIO_BUFFER_SIZE) {
        if (audio_codec_input(s_codec, chunk, chunk_samples)) {
            size_t bytes = chunk_samples * sizeof(int16_t);
            size_t copy_len = (total + bytes <= AUDIO_BUFFER_SIZE) ?
                               bytes : (AUDIO_BUFFER_SIZE - total);
            memcpy(s_buf->data + total, chunk, copy_len);
            total += copy_len;
        }
    }

    audio_codec_enable_input(s_codec, false);
    s_buf->size = total;
    s_recording = false;
    vTaskDelete(NULL);
}

/** 开始缓冲录音 */
void audio_recorder_start(audio_buffer_t *buf) {
    buf->data = heap_caps_malloc(AUDIO_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf->data) buf->data = malloc(AUDIO_BUFFER_SIZE);
    buf->size = 0;
    s_buf = buf;
    s_recording = true;
    xTaskCreatePinnedToCore(record_task, "record",
        TASK_RECORD_STACK_SIZE, NULL, TASK_RECORD_PRIORITY,
        &s_task_handle, TASK_RECORD_CORE);
}

/** 停止缓冲录音（等待任务完成） */
void audio_recorder_stop(void) {
    s_recording = false;
    while (s_task_handle != NULL && eTaskGetState(s_task_handle) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_task_handle = NULL;
}

bool audio_recorder_is_recording(void) {
    return s_recording && s_task_handle != NULL;
}

/**
 * @brief 流式录音任务
 *
 * 持续读取 stereo 音频帧，通过回调函数上传。
 * I2S RX 是 stereo（2 通道），需要读取 2x 样本数：
 *   read_samples = s_frame_samples * 2（如 960 * 2 = 1920 stereo 样本）
 * 回调函数接收 stereo 原始数据，负责提取左声道和 Opus 编码。
 *
 * 注意：I2S DMA 中断绑定在 Core 1，此任务必须在 Core 1 运行。
 */
static void stream_task(void *arg) {
    // I2S RX 是 stereo，需要读取 2x 样本数才能得到期望的 mono 时长
    int read_samples = s_frame_samples * 2;
    int16_t *chunk = malloc(read_samples * sizeof(int16_t));
    if (!chunk) {
        ESP_LOGE(TAG, "stream_task: malloc failed");
        s_streaming = false;
        vTaskDelete(NULL);
        return;
    }

    audio_codec_enable_input(s_codec, true);
    ESP_LOGI(TAG, "stream_task started, read_samples=%d (stereo), mono=%d", read_samples, s_frame_samples);

    int read_count = 0;
    int fail_count = 0;
    while (s_streaming) {
        if (audio_codec_input(s_codec, chunk, read_samples)) {
            read_count++;
            if (s_frame_cb) {
                // 传递 stereo 原始数据，回调负责 stereo→mono 转换和 Opus 编码
                s_frame_cb(chunk, read_samples, s_frame_ctx);
            }
            if (read_count <= 3 || read_count % 100 == 0) {
                ESP_LOGI(TAG, "stream_task: frame #%d sent (%d stereo samples)", read_count, read_samples);
            }
        } else {
            fail_count++;
            if (fail_count <= 5 || fail_count % 100 == 0) {
                ESP_LOGW(TAG, "stream_task: read fail #%d", fail_count);
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    audio_codec_enable_input(s_codec, false);
    free(chunk);
    ESP_LOGI(TAG, "stream_task ended, sent=%d fails=%d", read_count, fail_count);
    s_streaming = false;
    vTaskDelete(NULL);
}

/**
 * @brief 开始流式录音
 *
 * 创建独立任务持续读取音频帧，通过回调函数上传。
 * 调用方需要提供回调函数处理每个音频帧。
 *
 * @param cb 音频帧回调函数
 * @param user_ctx 用户上下文
 * @param frame_samples 每帧的 mono 样本数（如 960 = 60ms @ 16kHz）
 */
void audio_recorder_start_stream(audio_recorder_frame_cb_t cb, void *user_ctx, int frame_samples) {
    s_frame_cb = cb;
    s_frame_ctx = user_ctx;
    s_frame_samples = frame_samples;
    s_streaming = true;
    ESP_LOGI(TAG, "start_stream: creating task, samples=%d, stack=%d, prio=%d, core=%d",
             frame_samples, TASK_RECORD_STACK_SIZE, TASK_RECORD_PRIORITY, TASK_RECORD_CORE);
    BaseType_t ret = xTaskCreatePinnedToCore(stream_task, "stream",
        TASK_RECORD_STACK_SIZE, NULL, TASK_RECORD_PRIORITY,
        &s_stream_task_handle, TASK_RECORD_CORE);
    ESP_LOGI(TAG, "start_stream: xTaskCreate returned %ld, handle=%p", (long)ret, s_stream_task_handle);
}

/** 停止流式录音（等待任务完成） */
void audio_recorder_stop_stream(void) {
    s_streaming = false;
    while (s_stream_task_handle != NULL && eTaskGetState(s_stream_task_handle) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    s_stream_task_handle = NULL;
}

bool audio_recorder_is_streaming(void) {
    return s_streaming && s_stream_task_handle != NULL;
}