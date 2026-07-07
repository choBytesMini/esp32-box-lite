/**
 * @file codec_benchmark.h
 * @brief 音频硬件基准测试接口
 *
 * 测试扬声器和麦克风是否正常工作。
 */

#ifndef _CODEC_BENCHMARK_H_
#define _CODEC_BENCHMARK_H_

#include "audio_codec.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 基准测试结果 */
typedef struct {
    int tone_rms;       // 播放测试音调时麦克风采集的 RMS 电平
    int silence_rms;    // 静默时麦克风采集的 RMS 电平
    int mic_rms;        // 麦克风单独采集的 RMS 电平
    bool tone_detected; // 是否检测到播放的声音回环
    bool mic_ok;        // 麦克风是否工作正常
    bool spk_ok;        // 扬声器是否工作正常
} benchmark_result_t;

/** 运行音频基准测试 */
void codec_benchmark_run(audio_codec_t *codec, benchmark_result_t *result);

/** 打印测试结果 */
void codec_benchmark_print_result(const benchmark_result_t *result);

#ifdef __cplusplus
}
#endif

#endif // _CODEC_BENCHMARK_H_