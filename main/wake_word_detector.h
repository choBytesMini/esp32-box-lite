/**
 * @file wake_word_detector.h
 * @brief 唤醒词检测接口
 *
 * 基于 ESP-SR AFE + WakeNet 实现语音唤醒词检测。
 * 支持 "你好小智" 唤醒词。
 */

#ifndef _WAKE_WORD_DETECTOR_H_
#define _WAKE_WORD_DETECTOR_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化唤醒词检测（加载模型、配置 AFE） */
bool wake_word_init(uint32_t sample_rate);

/** 绑定音频编解码器 */
void wake_word_set_codec(audio_codec_t *codec);

/** 等待唤醒词检测（阻塞，直到检测到唤醒词） */
bool wake_word_wait(void);

/** 检查是否有语音活动（非阻塞） */
bool wake_word_is_voice_detected(void);

/** 检查唤醒词检测是否已初始化 */
bool wake_word_detector_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif // _WAKE_WORD_DETECTOR_H_