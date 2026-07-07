/**
 * @file opus_decoder.h
 * @brief Opus 解码器接口
 *
 * 用于服务器下发音频播放：WebSocket Opus 帧 → 解码 → PCM → I2S TX
 * 使用原生 libopus API（绕过 esp_opus_dec 封装层）
 */

#ifndef _OPUS_DECODER_H_
#define _OPUS_DECODER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 Opus 解码器（16kHz, mono） */
bool app_opus_decoder_init(void);

/** 释放 Opus 解码器 */
void app_opus_decoder_deinit(void);

/**
 * @brief 解码一帧 Opus 数据为 PCM
 *
 * @param opus_data      Opus 编码数据
 * @param opus_size      数据大小（字节）
 * @param pcm_out        输出 PCM 缓冲区
 * @param pcm_max_samples 最大样本数
 * @return 解码的样本数，失败返回 -1
 */
int  app_opus_decoder_decode(const uint8_t *opus_data, int opus_size,
                             int16_t *pcm_out, int pcm_max_samples);

#ifdef __cplusplus
}
#endif

#endif