#ifndef _AUDIO_CODEC_H_
#define _AUDIO_CODEC_H_

#include <driver/i2s_std.h>
#include <driver/i2c_master.h>
#include <driver/gpio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    i2s_chan_handle_t tx_handle;
    i2s_chan_handle_t rx_handle;
    void *codec_dev;  // esp_codec_dev_handle_t
    gpio_num_t pa_pin;
    bool output_enabled;
    bool input_enabled;
    int output_volume;
} audio_codec_t;

bool audio_codec_init(audio_codec_t *codec, i2c_master_bus_handle_t i2c_bus,
                      gpio_num_t pa_pin,
                      gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws,
                      gpio_num_t dout, gpio_num_t din);

void audio_codec_enable_output(audio_codec_t *codec, bool enable);
void audio_codec_enable_input(audio_codec_t *codec, bool enable);
void audio_codec_set_volume(audio_codec_t *codec, int volume);
bool audio_codec_output(audio_codec_t *codec, const int16_t *data, int samples);
bool audio_codec_input(audio_codec_t *codec, int16_t *data, int samples);

#ifdef __cplusplus
}
#endif

#endif // _AUDIO_CODEC_H_
