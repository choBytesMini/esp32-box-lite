/**
 * @file opus_decoder.c
 * @brief Opus 解码器封装 — 用于服务器下发音频播放
 *
 * 使用原生 libopus API（绕过 esp_opus_dec 封装层）
 * 原因：esp_opus_dec_decode 返回 -5，与服务器 opus_encode 输出不兼容
 *
 * libopus 符号已包含在 esp_audio_codec 的 libesp_audio_codec.a 中，
 * 通过前向声明调用，无需额外依赖。
 */

#include "opus_decoder.h"
#include "audio_stream_protocol.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "OpusDec";

/** libopus 前向声明（符号在 libesp_audio_codec.a 中） */
typedef struct OpusDecoder OpusDecoder;

int opus_decoder_get_size(int channels);
int opus_decoder_init(OpusDecoder *st, int Fs, int channels);
int opus_decode(OpusDecoder *st, const unsigned char *data,
                int len, short *pcm, int frame_size, int decode_fec);
const char *opus_strerror(int error);

static OpusDecoder *s_decoder = NULL;

/**
 * @brief 初始化 Opus 解码器
 *
 * 使用原生 libopus API，配置为 16kHz 单声道。
 * 解码器内存通过 malloc 分配。
 */
bool app_opus_decoder_init(void) {
    if (s_decoder) return true;

    int size = opus_decoder_get_size(AUDIO_STREAM_CHANNELS);  // 1 通道
    s_decoder = (OpusDecoder *)malloc(size);
    if (!s_decoder) {
        ESP_LOGE(TAG, "Failed to alloc Opus decoder (%d bytes)", size);
        return false;
    }

    int err = opus_decoder_init(s_decoder, AUDIO_STREAM_SAMPLE_RATE,  // 16kHz
                                 AUDIO_STREAM_CHANNELS);               // 1 通道
    if (err != 0) {
        ESP_LOGE(TAG, "Opus decoder init failed: %d (%s)", err, opus_strerror(err));
        free(s_decoder);
        s_decoder = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Opus decoder ready (native libopus, %d bytes)", size);
    return true;
}

/** 释放 Opus 解码器 */
void app_opus_decoder_deinit(void) {
    if (s_decoder) {
        free(s_decoder);
        s_decoder = NULL;
    }
}

/**
 * @brief 解码一帧 Opus 数据为 PCM
 *
 * @param opus_data    Opus 编码数据
 * @param opus_size    数据大小（字节）
 * @param pcm_out      输出 PCM 缓冲区
 * @param pcm_max_samples 最大样本数
 * @return 解码的样本数，失败返回 -1
 */
int app_opus_decoder_decode(const uint8_t *opus_data, int opus_size,
                            int16_t *pcm_out, int pcm_max_samples) {
    if (!s_decoder) return -1;

    int decoded = opus_decode(s_decoder, opus_data, opus_size,
                              pcm_out, pcm_max_samples, 0);
    if (decoded < 0) {
        static int err_count = 0;
        if (err_count < 5) {
            ESP_LOGE(TAG, "Opus decode failed: %d (%s), size=%d, first4=[%02x %02x %02x %02x]",
                     decoded, opus_strerror(decoded), opus_size,
                     opus_size > 0 ? opus_data[0] : 0,
                     opus_size > 1 ? opus_data[1] : 0,
                     opus_size > 2 ? opus_data[2] : 0,
                     opus_size > 3 ? opus_data[3] : 0);
            err_count++;
        }
        return -1;
    }

    return decoded;
}