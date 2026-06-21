idf_component_register(
    SRCS "main.c" "audio_codec.c" "lcd_display.c" "codec_benchmark.c"
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_jpeg/tjpgd"
    REQUIRES driver esp_lcd esp_timer freertos esp_driver_i2s esp_driver_i2c esp_driver_spi esp_adc espressif__esp_codec_dev espressif__esp_audio_codec espressif__esp_jpeg
    EMBED_FILES "test.jpg" "../output.mp3"
)
