/**
 * @file audio_player.c
 * @brief 音频播放模块 — URL 下载播放 + PCM 直接播放 + 流式队列播放
 *
 * 三种播放模式：
 *   1. URL 下载播放：HTTP 下载 WAV/MP3 → 跳过 44 字节头 → I2S TX 输出
 *   2. PCM 直接播放：直接写入 I2S TX（同步阻塞）
 *   3. 流式队列播放：FreeRTOS 队列 + 独立播放任务（用于服务器下发音频）
 *
 * 流式队列设计：
 *   - 队列存储在 PSRAM 中（内部 RAM 不够）
 *   - 每帧 960 个 int16_t 样本（60ms @ 16kHz mono）
 *   - 播放任务在 Core 0 运行（与 I2S RX 分离，避免 DMA 冲突）
 */

#include "audio_player.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

static const char *TAG = "AudioPlay";

#define PCM_FRAME_SAMPLES  960    // 每帧样本数（60ms @ 16kHz mono）
#define STREAM_QUEUE_DEPTH 64     // 队列深度

/** PCM 帧结构 */
typedef struct {
    int16_t data[PCM_FRAME_SAMPLES];
    size_t  samples;
} pcm_frame_t;

static audio_codec_t *s_codec = NULL;
static volatile bool s_playing = false;
static volatile bool s_stop_flag = false;

/** 流式播放队列（PSRAM 分配） */
static QueueHandle_t s_stream_queue = NULL;
static TaskHandle_t  s_stream_task = NULL;
static uint8_t *s_queue_storage = NULL;
static StaticQueue_t *s_queue_static = NULL;

/**
 * @brief 流式播放任务（Core 0）
 *
 * 从队列中读取 PCM 帧并写入 I2S TX。
 * 运行在 Core 0，与 I2S RX（Core 1）分离，避免 DMA 冲突。
 */
static void stream_play_task(void *arg) {
    pcm_frame_t frame;
    int play_count = 0;
    while (true) {
        if (xQueueReceive(s_stream_queue, &frame, pdMS_TO_TICKS(200)) == pdTRUE) {
            if (s_stop_flag) continue;
            s_playing = true;
            audio_codec_enable_output(s_codec, true);
            bool ok = audio_codec_output(s_codec, frame.data, frame.samples);
            if (++play_count <= 5 || play_count % 100 == 0) {
                ESP_LOGI(TAG, "stream_play: frame #%d samples=%d ok=%d", play_count, frame.samples, ok);
            }
        } else {
            s_playing = false;
        }
    }
}

/** 绑定音频编解码器 */
void audio_player_set_codec(audio_codec_t *codec) {
    s_codec = codec;
}

/**
 * @brief 从 URL 下载并播放音频
 *
 * 流程：HTTP GET → 下载到 PSRAM → 跳过 WAV 头（44 字节）→ I2S TX 输出
 * 注意：此函数阻塞当前任务，不应在 MQTT/WS 回调中调用。
 */
bool audio_player_play_from_url(const char *url) {
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = TTS_DOWNLOAD_TIMEOUT_MS,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > TTS_MAX_SIZE_BYTES) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    // 下载到 PSRAM
    uint8_t *buf = heap_caps_malloc(content_length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc(content_length);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return false;
    }

    int total = 0;
    int read_len;
    while ((read_len = esp_http_client_read(client, (char *)buf + total,
           content_length - total)) > 0) {
        total += read_len;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    // 跳过 WAV 头（44 字节）
    const uint8_t *pcm = buf;
    size_t pcm_size = total;
    if (total > 44 && buf[0] == 'R' && buf[1] == 'I') {
        pcm = buf + 44;
        pcm_size = total - 44;
    }

    // 同步播放
    s_stop_flag = false;
    s_playing = true;
    audio_codec_enable_output(s_codec, true);
    audio_codec_output(s_codec, (const int16_t *)pcm, pcm_size / 2);
    s_playing = false;

    free(buf);
    return true;
}

/** 直接播放 PCM 数据（同步阻塞） */
bool audio_player_play_pcm(const int16_t *data, size_t samples) {
    s_playing = true;
    audio_codec_enable_output(s_codec, true);
    bool ret = audio_codec_output(s_codec, data, samples);
    s_playing = false;
    return ret;
}

/**
 * @brief 启动流式播放队列
 *
 * 创建 PSRAM 队列 + 播放任务。
 * 调用后通过 audio_player_stream_queue() 入队 PCM 帧。
 */
bool audio_player_stream_start(void) {
    if (s_stream_queue) return true;
    // 用 PSRAM 分配队列存储（内部 RAM 只有 ~251KB 不够）
    size_t queue_size = STREAM_QUEUE_DEPTH * sizeof(pcm_frame_t);
    s_queue_storage = heap_caps_malloc(queue_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_queue_static = heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_queue_storage || !s_queue_static) {
        ESP_LOGE(TAG, "Failed to alloc queue in PSRAM");
        return false;
    }
    s_stream_queue = xQueueCreateStatic(STREAM_QUEUE_DEPTH, sizeof(pcm_frame_t),
                                         s_queue_storage, s_queue_static);
    if (!s_stream_queue) {
        ESP_LOGE(TAG, "Failed to create stream queue");
        return false;
    }
    s_stop_flag = false;
    xTaskCreatePinnedToCore(stream_play_task, "strm_play", 16384, NULL, 5, &s_stream_task, 0);
    ESP_LOGI(TAG, "Stream playback started (PSRAM queue, %dKB)", queue_size / 1024);
    return true;
}

/**
 * @brief 将 PCM 数据入队到流式播放队列
 *
 * 自动分帧（每帧 PCM_FRAME_SAMPLES 个样本），不足的用 0 填充。
 */
bool audio_player_stream_queue(const int16_t *data, size_t samples) {
    if (!s_stream_queue) return false;
    pcm_frame_t frame;
    size_t remaining = samples;
    size_t offset = 0;
    while (remaining > 0) {
        size_t chunk = (remaining > PCM_FRAME_SAMPLES) ? PCM_FRAME_SAMPLES : remaining;
        memcpy(frame.data, data + offset, chunk * sizeof(int16_t));
        if (chunk < PCM_FRAME_SAMPLES) {
            memset(frame.data + chunk, 0, (PCM_FRAME_SAMPLES - chunk) * sizeof(int16_t));
        }
        frame.samples = chunk;
        if (xQueueSend(s_stream_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "Stream queue full, dropping frame");
            return false;
        }
        offset += chunk;
        remaining -= chunk;
    }
    return true;
}

/** 停止流式播放（清空队列） */
void audio_player_stream_stop(void) {
    s_stop_flag = true;
    if (s_stream_queue) {
        xQueueReset(s_stream_queue);
    }
}

/** 停止所有播放并销毁流式播放任务（避免 DMA 冲突） */
void audio_player_stop(void) {
    s_stop_flag = true;
    s_playing = false;

    // 销毁流式播放任务
    if (s_stream_task) {
        vTaskDelete(s_stream_task);
        s_stream_task = NULL;
    }

    // 清空并释放队列
    if (s_stream_queue) {
        xQueueReset(s_stream_queue);
        s_stream_queue = NULL;
    }
    if (s_queue_storage) {
        free(s_queue_storage);
        s_queue_storage = NULL;
    }
    if (s_queue_static) {
        free(s_queue_static);
        s_queue_static = NULL;
    }

    ESP_LOGI(TAG, "Audio player stopped, stream task deleted");
}

/** 检查是否正在播放 */
bool audio_player_is_playing(void) {
    return s_playing;
}