/**
 * @file opus_encoder.c
 * @brief Opus 编码器封装 — 用于麦克风音频上传
 *
 * 配置：16kHz, 单声道, 60ms 帧, VBR, DTX
 * 每帧 960 个样本（16000 * 0.06），输出最大 256 字节
 *
 * 注意：使用 ESP-IDF 的 esp_opus_enc 封装层
 */

#include "opus_encoder.h"
#include "audio_stream_protocol.h"
#include "esp_opus_enc.h"
#include "esp_audio_enc.h"
#include "esp_audio_types.h"
#include "esp_log.h"

static const char *TAG = "OpusEnc";
static void *s_encoder = NULL;       // 编码器句柄
static int s_frame_samples = 0;      // 每帧样本数（960）
static int s_outbuf_size = 0;        // 输出缓冲区大小

bool app_opus_encoder_init(void) {
    if (s_encoder) return true;

    // Opus 编码器配置
    esp_opus_enc_config_t cfg = {
        .sample_rate      = ESP_AUDIO_SAMPLE_RATE_16K,
        .channel          = ESP_AUDIO_MONO,
        .bits_per_sample  = ESP_AUDIO_BIT16,
        .bitrate          = ESP_OPUS_BITRATE_AUTO,
        .frame_duration   = ESP_OPUS_ENC_FRAME_DURATION_60_MS,  // 60ms 帧
        .application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO,
        .complexity       = 0,       // 最低复杂度
        .enable_fec       = false,   // 不使用前向纠错
        .enable_dtx       = true,    // 启用不连续传输（静音时不编码）
        .enable_vbr       = true,    // 启用可变比特率
    };

    int ret = esp_opus_enc_open(&cfg, sizeof(cfg), &s_encoder);
    if (s_encoder == NULL) {
        ESP_LOGE(TAG, "Failed to create Opus encoder: %d", ret);
        return false;
    }

    // 获取帧大小和输出缓冲区大小
    int frame_size_bytes = 0;
    esp_opus_enc_get_frame_size(s_encoder, &frame_size_bytes, &s_outbuf_size);
    s_frame_samples = frame_size_bytes / sizeof(int16_t);

    ESP_LOGI(TAG, "Opus encoder ready: frame=%d samples, outbuf=%d bytes",
             s_frame_samples, s_outbuf_size);
    return true;
}

void app_opus_encoder_deinit(void) {
    if (s_encoder) {
        esp_opus_enc_close(s_encoder);
        s_encoder = NULL;
    }
}

int app_opus_encoder_get_outbuf_size(void) {
    return s_outbuf_size;
}

int app_opus_encoder_get_frame_samples(void) {
    return s_frame_samples;
}

/**
 * @brief 编码一帧 PCM 数据为 Opus
 *
 * @param pcm      输入 PCM 数据（mono, 16-bit）
 * @param pcm_samples 样本数（必须等于 s_frame_samples = 960）
 * @param out      输出缓冲区
 * @param out_max  输出缓冲区大小
 * @return 编码后的字节数，失败返回 -1
 */
int app_opus_encoder_encode(const int16_t *pcm, int pcm_samples, uint8_t *out, int out_max) {
    if (!s_encoder) return -1;
    if (pcm_samples != s_frame_samples) {
        ESP_LOGE(TAG, "Frame size mismatch: got %d, expect %d", pcm_samples, s_frame_samples);
        return -1;
    }

    esp_audio_enc_in_frame_t in = {
        .buffer = (uint8_t *)pcm,
        .len    = (uint32_t)(s_frame_samples * sizeof(int16_t)),
    };
    esp_audio_enc_out_frame_t out_frame = {
        .buffer        = out,
        .len           = (uint32_t)out_max,
        .encoded_bytes = 0,
    };

    int ret = esp_opus_enc_process(s_encoder, &in, &out_frame);
    if (ret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Opus encode failed: %d", ret);
        return -1;
    }

    return (int)out_frame.encoded_bytes;
}