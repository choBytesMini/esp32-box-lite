/**
 * @file config.h
 * @brief ESP32-S3-BOX-Lite 全局配置文件
 *
 * 包含所有硬件引脚、网络配置、音频参数、任务配置等。
 * 所有模块通过 #include "config.h" 获取配置。
 */

#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include "driver/gpio.h"
#include "hal/adc_types.h"

// ======================== WiFi 配置 ========================
#define WIFI_SSID                      "chobits-2.4G"
#define WIFI_PASSWORD                  "chobytes007"
#define WIFI_TIMEOUT_MS                15000   // WiFi 连接超时（毫秒）

// ======================== 服务器配置 ========================
#define SERVER_HOST                    "82.158.224.81"   // 云服务器 IP
#define MQTT_HOST                      "82.158.224.81"
#define SERVER_HTTP_PORT               5000    // HTTP API 端口
#define MQTT_PORT                      1883    // MQTT 端口

// ======================== MQTT Topic ========================
// 服务器通过这些 topic 下发指令和回复
#define MQTT_TOPIC_REPLY               "home/agent/reply"       // AI 回复
#define MQTT_TOPIC_IDENTITY            "home/agent/identity"    // 声纹识别结果
#define MQTT_TOPIC_ALERT               "home/alert"             // 告警通知
#define MQTT_TOPIC_STATUS              "home/status/esp32"      // 设备状态上报
#define MQTT_TOPIC_MUSIC               "home/music/control"     // 音乐控制
#define MQTT_TOPIC_MUSIC_STATUS        "home/music/status"      // 音乐状态
#define MQTT_TOPIC_SKILL               "home/agent/skill"       // 技能调用
#define MQTT_CLIENT_ID_PREFIX          "esp32_living_room"
#define MQTT_BUFFER_SIZE               2048
#define MQTT_CONNECT_RETRIES           5
#define MQTT_RETRY_DELAY_MS            2000

// ======================== 音频参数 ========================
#define AUDIO_INPUT_SAMPLE_RATE  16000   // 麦克风采样率（Hz）
#define AUDIO_OUTPUT_SAMPLE_RATE 44100   // 扬声器采样率（Hz）
#define AUDIO_MAX_DURATION_MS    5000    // 最大录音时长（毫秒）
#define AUDIO_BUFFER_SIZE        (AUDIO_INPUT_SAMPLE_RATE * 2 * AUDIO_MAX_DURATION_MS / 1000)  // 录音缓冲区大小（字节）
#define AUDIO_MIN_VALID_BYTES    3200    // 最小有效音频字节数

// ======================== TTS 配置 ========================
#define TTS_DOWNLOAD_TIMEOUT_MS  10000   // TTS 下载超时（毫秒）
#define TTS_MAX_SIZE_BYTES       500000  // TTS 文件最大大小（字节）

// ======================== WebSocket 配置 ========================
#define WS_URL                       "ws://82.158.224.81:8888/ws"  // WebSocket 地址
#define WS_SEND_TIMEOUT_MS           5000   // 发送超时（毫秒）

// ======================== HTTP 配置 ========================
#define HTTP_UPLOAD_TIMEOUT_MS   10000   // HTTP 上传超时（毫秒）

// ======================== I2S 引脚（BOX Lite 硬件定义） ========================
// MCLK=GPIO2, BCLK=GPIO17, WS=GPIO47, DOUT=GPIO15, DIN=GPIO16
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_2    // 主时钟
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_47   // 字选择（左右声道切换）
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_17   // 位时钟
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_16   // 数据输入（麦克风）
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_15   // 数据输出（扬声器）

// ======================== 音频编解码器 ========================
#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46  // 功放使能引脚
#define AUDIO_CODEC_I2C_SDA_PIN  GPIO_NUM_8   // I2C 数据线
#define AUDIO_CODEC_I2C_SCL_PIN  GPIO_NUM_18  // I2C 时钟线

// ======================== LCD 引脚（ILI9341 SPI） ========================
#define LCD_SPI_CS_PIN    GPIO_NUM_5   // 片选
#define LCD_SPI_DC_PIN    GPIO_NUM_4   // 数据/命令选择
#define LCD_SPI_RST_PIN   GPIO_NUM_48  // 复位
#define LCD_SPI_MOSI_PIN  GPIO_NUM_6   // 主出从入
#define LCD_SPI_SCLK_PIN  GPIO_NUM_7   // SPI 时钟
#define LCD_BACKLIGHT_PIN GPIO_NUM_45  // 背光

// ======================== LCD 参数 ========================
#define LCD_WIDTH   320
#define LCD_HEIGHT  240

// ======================== 按键 ADC 配置 ========================
// BOX Lite 的 3 个按键通过 ADC 电压检测（非 GPIO）
#define BUTTON_ADC_UNIT    ADC_UNIT_1
#define BUTTON_ADC_CHANNEL ADC_CHANNEL_0
#define BUTTON_ADC_GPIO    GPIO_NUM_1

// 按键电压阈值 (mV) — 中心值 ± 100mV
#define BUTTON_PREV_MV_MIN   2310   // 上一首（中心 2410mV）
#define BUTTON_PREV_MV_MAX   2510
#define BUTTON_ENTER_MV_MIN  1880   // 确认键（中心 1980mV）
#define BUTTON_ENTER_MV_MAX  2080
#define BUTTON_NEXT_MV_MIN   720    // 下一首（中心 820mV）
#define BUTTON_NEXT_MV_MAX   920

// ======================== 音量控制 ========================
#define VOLUME_DEFAULT   100     // 默认音量（%）
#define VOLUME_STEP      10     // 每次调节步长
#define VOLUME_MIN       0
#define VOLUME_MAX       100

// ======================== FreeRTOS 任务配置 ========================
// 唤醒词检测任务：Core 1, 优先级 4, 栈 16KB
#define TASK_WAKE_WORD_STACK_SIZE  16384
#define TASK_WAKE_WORD_PRIORITY    4
#define TASK_WAKE_WORD_CORE        1

// 录音任务：Core 1, 优先级 7, 栈 32KB
// 注意：必须在 Core 1（I2S DMA 中断绑定在 Core 1）
#define TASK_RECORD_STACK_SIZE     32768
#define TASK_RECORD_PRIORITY       7
#define TASK_RECORD_CORE           1

// ======================== 基准测试参数 ========================
#define BENCHMARK_TONE_FREQ     1000   // 测试音调频率（Hz）
#define BENCHMARK_TONE_DURATION 3000   // 测试音调时长（ms）
#define BENCHMARK_TONE_SR       44100  // 测试音调采样率
#define BENCHMARK_MIC_DURATION  3000   // 麦克风测试时长（ms）
#define BENCHMARK_MIC_SR        16000  // 麦克风采样率
#define BENCHMARK_VOICE_THRESH  500    // 声音检测阈值

#endif // _BOARD_CONFIG_H_