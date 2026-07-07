/**
 * @file opus_encoder.h
 * @brief Opus 编码器接口
 *
 * 用于麦克风音频上传：PCM → Opus 编码 → BP3 帧 → WebSocket 发送
 * 配置：16kHz, mono, 60ms 帧, VBR
 */

#ifndef _OPUS_ENCODER_H_
#define _OPUS_ENCODER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 Opus 编码器 */
bool app_opus_encoder_init(void);

/** 释放 Opus 编码器 */
void app_opus_encoder_deinit(void);

/** 获取输出缓冲区大小 */
int  app_opus_encoder_get_outbuf_size(void);

/** 编码一帧 PCM 数据为 Opus */
int  app_opus_encoder_encode(const int16_t *pcm, int pcm_samples, uint8_t *out, int out_max);

/** 获取每帧样本数（960 = 60ms @ 16kHz） */
int  app_opus_encoder_get_frame_samples(void);

#ifdef __cplusplus
}
#endif

#endif