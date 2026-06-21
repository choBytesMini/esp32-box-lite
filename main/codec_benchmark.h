#ifndef _CODEC_BENCHMARK_H_
#define _CODEC_BENCHMARK_H_

#include "audio_codec.h"
#include "lcd_display.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int tone_rms;       // 播放测试音调时麦克风采集的RMS电平
    int silence_rms;    // 静默时麦克风采集的RMS电平
    int mic_rms;        // 麦克风单独采集的RMS电平
    bool tone_detected; // 是否检测到播放的声音回环
    bool mic_ok;        // 麦克风是否工作正常
    bool spk_ok;        // 扬声器是否工作正常
} benchmark_result_t;

void codec_benchmark_run(audio_codec_t *codec, lcd_display_t *lcd,
                         benchmark_result_t *result);

void codec_benchmark_print_result(const benchmark_result_t *result);

#ifdef __cplusplus
}
#endif

#endif // _CODEC_BENCHMARK_H_
