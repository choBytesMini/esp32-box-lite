#include "codec_benchmark.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <math.h>
#include <string.h>

static const char *TAG = "Benchmark";

static void generate_sine_tone(int16_t *buf, int samples, int freq, int sr) {
    for (int i = 0; i < samples; i++) {
        float t = (float)i / sr;
        float env = 1.0f;
        int fade = sr / 20;
        if (i < fade) env = (float)i / fade;
        if (i > samples - fade) env = (float)(samples - i) / fade;
        buf[i] = (int16_t)(20000 * sinf(2.0f * M_PI * freq * t) * env);
    }
}

static int measure_mic_level(audio_codec_t *codec, int duration_ms) {
    return audio_codec_detect_voice(codec, duration_ms);
}

void codec_benchmark_run(audio_codec_t *codec, lcd_display_t *lcd,
                         benchmark_result_t *result) {
    memset(result, 0, sizeof(benchmark_result_t));

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Codec Benchmark 开始");
    ESP_LOGI(TAG, "========================================");

    // 阶段1: 静默环境噪声测量
    ESP_LOGI(TAG, "[1/4] 测量环境噪声...");
    audio_codec_enable_input(codec, true);
    result->silence_rms = measure_mic_level(codec, BENCHMARK_MIC_DURATION);
    ESP_LOGI(TAG, "  环境噪声 RMS: %d", result->silence_rms);

    // 阶段2: 扬声器播放测试
    ESP_LOGI(TAG, "[2/4] 播放 %dHz 测试音调 (%d秒)...",
             BENCHMARK_TONE_FREQ, BENCHMARK_TONE_DURATION / 1000);

    int tone_samples = BENCHMARK_TONE_SR * BENCHMARK_TONE_DURATION / 1000;
    int16_t *tone_buf = heap_caps_malloc(tone_samples * sizeof(int16_t),
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tone_buf) tone_buf = malloc(tone_samples * sizeof(int16_t));

    if (tone_buf) {
        generate_sine_tone(tone_buf, tone_samples,
                           BENCHMARK_TONE_FREQ, BENCHMARK_TONE_SR);
        audio_codec_enable_output(codec, true);
        audio_codec_output(codec, tone_buf, tone_samples);
        free(tone_buf);
        ESP_LOGI(TAG, "  播放完成");
    } else {
        ESP_LOGE(TAG, "  内存分配失败");
    }

    // 阶段3: 扬声器播放时同步麦克风采集
    ESP_LOGI(TAG, "[3/4] 播放 + 麦克风回环采集...");
    tone_samples = BENCHMARK_TONE_SR * BENCHMARK_TONE_DURATION / 1000;
    tone_buf = heap_caps_malloc(tone_samples * sizeof(int16_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!tone_buf) tone_buf = malloc(tone_samples * sizeof(int16_t));

    if (tone_buf) {
        generate_sine_tone(tone_buf, tone_samples,
                           BENCHMARK_TONE_FREQ, BENCHMARK_TONE_SR);

        // 启动麦克风采集任务
        int loop_rms = 0;
        audio_codec_enable_output(codec, true);
        audio_codec_enable_input(codec, true);

        // 先播放一小段让功放稳定
        audio_codec_output(codec, tone_buf,
                          BENCHMARK_TONE_SR / 10);  // 100ms
        vTaskDelay(pdMS_TO_TICKS(50));

        // 测量
        loop_rms = measure_mic_level(codec, BENCHMARK_MIC_DURATION);

        // 继续播放剩余部分
        audio_codec_output(codec, tone_buf, tone_samples);
        result->tone_rms = loop_rms;
        free(tone_buf);
        ESP_LOGI(TAG, "  回环 RMS: %d", result->tone_rms);
    }

    // 阶段4: 麦克风单独测试
    ESP_LOGI(TAG, "[4/4] 麦克风单独采集 (%d秒)...",
             BENCHMARK_MIC_DURATION / 1000);
    audio_codec_enable_output(codec, false);
    audio_codec_enable_input(codec, true);
    result->mic_rms = measure_mic_level(codec, BENCHMARK_MIC_DURATION);
    ESP_LOGI(TAG, "  麦克风 RMS: %d", result->mic_rms);

    // 判定结果
    result->mic_ok = (result->mic_rms > BENCHMARK_VOICE_THRESH) ||
                     (result->silence_rms < 100);
    result->tone_detected = (result->tone_rms > result->silence_rms * 2) &&
                            (result->tone_rms > BENCHMARK_VOICE_THRESH);
    result->spk_ok = result->tone_detected;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, " Codec Benchmark 完成");
    ESP_LOGI(TAG, "========================================");

    codec_benchmark_print_result(result);
}

void codec_benchmark_print_result(const benchmark_result_t *result) {
    ESP_LOGI(TAG, "--- 测试结果 ---");
    ESP_LOGI(TAG, "  环境噪声 RMS:  %d", result->silence_rms);
    ESP_LOGI(TAG, "  回环 RMS:      %d", result->tone_rms);
    ESP_LOGI(TAG, "  麦克风 RMS:    %d", result->mic_rms);
    ESP_LOGI(TAG, "  扬声器状态:    %s", result->spk_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  麦克风状态:    %s", result->mic_ok ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  回环检测:      %s", result->tone_detected ? "OK" : "FAIL");
    ESP_LOGI(TAG, "  语音阈值:      %d", BENCHMARK_VOICE_THRESH);
    ESP_LOGI(TAG, "--- 判定标准 ---");
    ESP_LOGI(TAG, "  RMS < 200:     安静/无声音");
    ESP_LOGI(TAG, "  RMS 200-500:   微弱声音/环境噪声");
    ESP_LOGI(TAG, "  RMS > 500:     有明显声音");
    ESP_LOGI(TAG, "  RMS > 2000:    较大声音");
}
