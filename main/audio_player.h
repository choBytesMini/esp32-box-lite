/**
 * @file audio_player.h
 * @brief 音频播放接口
 *
 * 支持三种播放模式：
 *   1. URL 下载播放（HTTP GET → PCM → I2S TX）
 *   2. PCM 直接播放（同步阻塞）
 *   3. 流式队列播放（FreeRTOS 队列 + 独立任务）
 */

#ifndef _AUDIO_PLAYER_H_
#define _AUDIO_PLAYER_H_

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "audio_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 绑定音频编解码器 */
void audio_player_set_codec(audio_codec_t *codec);

/** 从 URL 下载并播放音频（同步阻塞） */
bool audio_player_play_from_url(const char *url);

/** 直接播放 PCM 数据（同步阻塞） */
bool audio_player_play_pcm(const int16_t *data, size_t samples);

/** 启动流式播放队列 */
bool audio_player_stream_start(void);

/** 将 PCM 数据入队到流式播放队列 */
bool audio_player_stream_queue(const int16_t *data, size_t samples);

/** 停止流式播放 */
void audio_player_stream_stop(void);

/** 停止所有播放 */
void audio_player_stop(void);

/** 检查是否正在播放 */
bool audio_player_is_playing(void);

#ifdef __cplusplus
}
#endif

#endif // _AUDIO_PLAYER_H_