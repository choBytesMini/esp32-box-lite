idf_component_register(
    SRCS "main.c" "audio_codec.c" "lcd_display.c" "codec_benchmark.c"
         "wifi_manager.c" "app_mqtt_client.c" "audio_recorder.c" "audio_player.c"
         "http_uploader.c" "wake_word_detector.c"
         "opus_encoder.c" "opus_decoder.c" "websocket_uploader.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd esp_timer freertos
             esp_driver_i2s esp_driver_i2c esp_driver_spi esp_adc
             espressif__esp_codec_dev espressif__esp_audio_codec
             esp_wifi esp_netif esp_http_client nvs_flash
             espressif__esp-sr espressif__mqtt espressif__cjson
             tcp_transport
    EMBED_FILES "../output.mp3"
)
