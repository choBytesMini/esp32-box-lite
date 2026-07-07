/**
 * @file audio_recorder.h
 * @brief 音频录制接口
 *
 * 支持两种录制模式：
 *   1. 缓冲录音：一次性录制固定时长的音频到缓冲区
 *   2. 流式录音：持续读取音频帧并通过回调上传
 */

#ifndef _AUDIO_RECORDER_H_
#define _AUDIO_RECORDER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 音频缓冲区结构 */
typedef struct {
    uint8_t *data;   // 数据指针
    size_t   size;   // 实际数据大小
} audio_buffer_t;

/** 音频帧回调函数类型 */
typedef void (*audio_recorder_frame_cb_t)(const int16_t *pcm, int samples, void *user_ctx);

/** 绑定音频编解码器 */
void audio_recorder_set_codec(audio_codec_t *codec);

/** 开始缓冲录音 */
void audio_recorder_start(audio_buffer_t *buf);

/** 停止缓冲录音 */
void audio_recorder_stop(void);

/** 检查是否正在缓冲录音 */
bool audio_recorder_is_recording(void);

/** 开始流式录音（回调模式） */
void audio_recorder_start_stream(audio_recorder_frame_cb_t cb, void *user_ctx, int frame_samples);

/** 停止流式录音 */
void audio_recorder_stop_stream(void);

/** 检查是否正在流式录音 */
bool audio_recorder_is_streaming(void);

#ifdef __cplusplus
}
#endif

#endif