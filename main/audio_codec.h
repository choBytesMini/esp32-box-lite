/**
 * @file audio_codec.h
 * @brief 音频编解码器接口
 *
 * 管理 ESP32-S3-BOX-Lite 的 I2S TX/RX 通道和 ES8156/ES7243E 编解码器。
 * 所有 I2S 操作通过 mutex 保护，防止 DMA 并发死锁。
 */

#ifndef _AUDIO_CODEC_H_
#define _AUDIO_CODEC_H_

#include <driver/i2s_std.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>
#include <esp_codec_dev.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 音频编解码器状态结构 */
typedef struct {
    i2s_chan_handle_t tx_handle;      // I2S TX 通道句柄
    i2s_chan_handle_t rx_handle;      // I2S RX 通道句柄
    void *codec_dev;                  // esp_codec_dev_handle_t（保留）
    gpio_num_t pa_pin;                // 功放 GPIO 引脚
    bool output_enabled;              // 输出启用标志
    bool input_enabled;               // 输入启用标志
    int output_volume;                // 输出音量（0-100）
} audio_codec_t;

/** 初始化音频编解码器（I2S + I2C） */
bool audio_codec_init(audio_codec_t *codec, i2c_master_bus_handle_t i2c_bus,
                      gpio_num_t pa_pin,
                      gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                      gpio_num_t dout, gpio_num_t din);

/** 启用/禁用音频输出 */
void audio_codec_enable_output(audio_codec_t *codec, bool enable);

/** 启用/禁用音频输入 */
void audio_codec_enable_input(audio_codec_t *codec, bool enable);

/** 设置输出音量（0-100） */
void audio_codec_set_volume(audio_codec_t *codec, int volume);

/** 写音频数据到 I2S TX（扬声器输出） */
bool audio_codec_output(audio_codec_t *codec, const int16_t *data, int samples);

/** 从 I2S RX 读取音频数据（麦克风输入，返回 stereo） */
bool audio_codec_input(audio_codec_t *codec, int16_t *data, int samples);

/** 检测指定时长内的声音 RMS 电平 */
int audio_codec_detect_voice(audio_codec_t *codec, int duration_ms);

#ifdef __cplusplus
}
#endif

#endif // _AUDIO_CODEC_H_