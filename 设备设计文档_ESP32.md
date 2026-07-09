# ESP32-S3-BOX-Lite 设备设计文档（C / ESP-IDF）

> 职责：客厅交互终端（语音唤醒 + Opus编码上传 + 服务器音频推送播放 + LCD显示）
> 框架：ESP-IDF v6.0.1 (官方原生开发)
> 语言：C

---

## 1. 设备概述

ESP32-S3-BOX-Lite 在系统中担任**纯交互终端**，不做任何 AI 推理：

```
用户说"你好小安" → 唤醒词检测 → 流式录音 → Opus编码 → WebSocket上传到服务器
→ 服务器STT→LLM→TTS → WebSocket推送PCM音频 → ESP32扬声器播放
→ 15秒静默超时 → 回到等待唤醒词
```

## 2. 硬件资源

```
板载 (无需外接):
  - ESP32-S3: Xtensa LX7 双核 240MHz, 512KB SRAM, 8MB Flash, 8MB PSRAM
  - LCD: 2.4" 320x240 SPI ILI9341 (esp_lcd_panel 驱动)
  - 麦克风: ES7243E ADC (I2S0 RX, 采样率 16kHz, 16bit, stereo)
    → 用于唤醒词检测 + 语音录制（提取左声道 mono）
  - 喇叭: ES8156 DAC (I2S0 TX, 采样率 16kHz, 16bit, mono)
  - 功放: GPIO46 (HIGH=开)
  - BOOT 按键: ADC1_CH0 (GPIO1, 三个按键共用 ADC 分压)

硬件引脚:
  LCD SPI:  SCLK=7, MOSI=6, DC=4, CS=5, RST=48, BL=45
  I2S:      MCLK=38, BCLK=14, WS=13, DOUT=11, DIN=12
  I2C:      SDA=8, SCL=18 (ES8156 + ES7243E)
  按键 ADC: GPIO1 (ADC1_CH0)

注意事项:
  - LCD 驱动芯片为 ILI9341 (不是 ST7789V)
  - 背光引脚 GPIO45 需要反转控制 (LOW=亮, HIGH=灭)
  - 音频编解码器通过 I2C 控制 (ES8156 addr=0x10, ES7243E addr=0x20)
  - 功放引脚 GPIO46 HIGH=开 (pa_reverted=false)
  - I2S DMA 中断绑定 Core 1，所有 I2S 读写任务必须在 Core 1
  - 蜂鸣器已删除（GPIO46 与功放共用）
```

## 3. 开发环境与软件架构

### 3.1 开发工具：VSCode + ESP-IDF

```
为什么用 ESP-IDF 原生开发:
  - 官方驱动完整支持 (esp_lcd, esp_codec_dev, esp-sr 等)
  - Arduino 框架缺少 ES8156/ES7243E 等 codec 驱动
  - 直接使用 ESP-SR 语音识别 (唤醒词检测)
  - 更精细的内存管理 (PSRAM, DMA)
  - 更好的 FreeRTOS 集成
  - 官方组件管理器 (idf_component.yml)

安装步骤:
  1. 安装 ESP-IDF v6.0.1 (https://docs.espressif.com/projects/esp-idf/)
  2. VSCode 安装 ESP-IDF 插件
  3. idf.py set-target esp32s3
  4. idf.py build / flash / monitor
```

### 3.2 ESP-IDF 项目结构

```
esp32_box_lite/
  CMakeLists.txt                  // 项目 CMake 配置
  sdkconfig.defaults              // SDK 默认配置
  partitions.csv                  // 分区表 (model 960KB + factory 7MB)
  main/
    CMakeLists.txt                // 组件 CMake 配置
    idf_component.yml             // 组件依赖声明
    config.h                      // 全局配置常量（引脚、网络、音频参数）
    main.c                        // 入口 (app_main)，WebSocket回调，唤醒词任务
    wifi_manager.h / .c           // Wi-Fi 连接管理
    app_mqtt_client.h / .c        // MQTT 消息收发
    audio_recorder.h / .c         // I2S 麦克风录音（缓冲+流式）
    audio_player.h / .c           // 流式播放（PSRAM队列 + stream_play_task）
    audio_codec.h / .c            // I2S 初始化，ES8156+ES7243E 编解码器
    audio_stream_protocol.h       // BP3 帧格式定义
    opus_encoder.h / .c           // Opus 编码器
    opus_decoder.h / .c           // Opus 解码器（原生 libopus API）
    websocket_uploader.h / .c     // WebSocket 客户端（BP3帧收发）
    http_uploader.h / .c          // HTTP 客户端
    lcd_display.h / .c            // LCD 显示 (esp_lcd_panel，聊天UI)
    wake_word_detector.h / .c     // 唤醒词检测 (ESP-SR AFE + WakeNet)
```

### 3.3 依赖组件

```
ESP-IDF 官方组件 (idf_component.yml):
  espressif/esp_lcd_ili9341    ~2.0.0    LCD 驱动
  espressif/esp_codec_dev      ~1.5.6    音频编解码器抽象
  espressif/esp_audio_codec    ~2.4.1    音频编解码 (Opus/MP3)
  espressif/esp-sr             ~2.4.6    语音识别 (唤醒词 + AFE)
  espressif/mqtt               ^1.0.0    MQTT 客户端
  espressif/cjson              ^1.7.18   JSON 解析

ESP-IDF 内置组件 (无需声明):
  esp_wifi                     Wi-Fi 驱动
  esp_netif                    网络接口抽象
  esp_http_client              HTTP 客户端
  driver/i2s_std               I2S 驱动 (新版 API)
  driver/gpio                  GPIO 驱动
  esp_lcd                      LCD 面板抽象
  freertos                     FreeRTOS 任务管理
  tcp_transport                WebSocket 底层传输
```

## 4. 详细设计

### 4.1 sdkconfig.defaults — SDK 配置

```ini
# ESP32-S3 配置
CONFIG_IDF_TARGET="esp32s3"

# Flash 配置
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHMODE_DIO=y

# PSRAM 配置
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# 分区表
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# USB 串口
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y

# 日志
CONFIG_LOG_DEFAULT_LEVEL_INFO=y

# Wi-Fi 优化
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=6
CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM=8

# mbedTLS (HTTPS 支持)
CONFIG_MBEDTLS_DYNAMIC_BUFFER=y
CONFIG_MBEDTLS_SSL_RENEGOTIATION=y

# 唤醒词模型 (你好小安)
CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2=y
```

### 4.2 idf_component.yml — 组件依赖

```yaml
dependencies:
  espressif/esp_lcd_ili9341: ~2.0.0
  espressif/esp_codec_dev: ~1.5.6
  espressif/esp_audio_codec: ~2.4.1
  espressif/esp-sr: ~2.4.6
  espressif/mqtt: ^1.0.0
  espressif/cjson: ^1.7.18
```

### 4.3 config.h — 全局配置

```c
#pragma once
#include "driver/gpio.h"

// ======================== WiFi 配置 ========================
#define WIFI_SSID                      "chobits"
#define WIFI_PASSWORD                  "chobytes"
#define WIFI_TIMEOUT_MS                15000

// ======================== 服务器配置 ========================
#define SERVER_HOST                    "82.158.224.81"
#define MQTT_HOST                      "82.158.224.81"
#define SERVER_HTTP_PORT               5000
#define MQTT_PORT                      1883
#define WS_URL                         "ws://82.158.224.81:8888/ws"
#define WS_SEND_TIMEOUT_MS             5000

// ======================== MQTT Topic ========================
#define MQTT_TOPIC_REPLY               "home/agent/reply"
#define MQTT_TOPIC_IDENTITY            "home/agent/identity"
#define MQTT_TOPIC_ALERT               "home/alert"
#define MQTT_TOPIC_STATUS              "home/status/esp32"
#define MQTT_TOPIC_MUSIC               "home/music/control"
#define MQTT_TOPIC_MUSIC_STATUS        "home/music/status"
#define MQTT_TOPIC_SKILL               "home/agent/skill"
#define MQTT_CLIENT_ID_PREFIX          "esp32_living_room"

// ======================== I2S 引脚 ========================
#define AUDIO_I2S_GPIO_MCLK            GPIO_NUM_38
#define AUDIO_I2S_GPIO_BCLK            GPIO_NUM_14
#define AUDIO_I2S_GPIO_WS              GPIO_NUM_13
#define AUDIO_I2S_GPIO_DOUT            GPIO_NUM_11
#define AUDIO_I2S_GPIO_DIN             GPIO_NUM_12

// ======================== 音频编解码器 ========================
#define AUDIO_CODEC_PA_PIN             GPIO_NUM_46
#define AUDIO_CODEC_I2C_SDA_PIN        GPIO_NUM_8
#define AUDIO_CODEC_I2C_SCL_PIN        GPIO_NUM_18

// ======================== LCD SPI 引脚 ========================
#define LCD_SPI_CS_PIN                 GPIO_NUM_5
#define LCD_SPI_DC_PIN                 GPIO_NUM_4
#define LCD_SPI_RST_PIN                GPIO_NUM_48
#define LCD_SPI_MOSI_PIN               GPIO_NUM_6
#define LCD_SPI_SCLK_PIN               GPIO_NUM_7
#define LCD_BACKLIGHT_PIN              GPIO_NUM_45

// ======================== LCD 参数 ========================
#define LCD_WIDTH                      320
#define LCD_HEIGHT                     240

// ======================== 音频参数 ========================
#define AUDIO_INPUT_SAMPLE_RATE        16000
#define AUDIO_OUTPUT_SAMPLE_RATE       16000
#define AUDIO_BITS_PER_SAMPLE          16
#define AUDIO_MAX_DURATION_MS          15000
#define AUDIO_BUFFER_SIZE              (AUDIO_INPUT_SAMPLE_RATE * 2 * AUDIO_MAX_DURATION_MS / 1000)
#define AUDIO_MIN_VALID_BYTES          3200

// ======================== Opus 编码 ========================
#define AUDIO_STREAM_FRAME_DURATION_MS 60
#define AUDIO_STREAM_FRAME_SAMPLES     960   // 16000 * 60 / 1000

// ======================== TTS 配置 ========================
#define TTS_DOWNLOAD_TIMEOUT_MS        10000
#define TTS_MAX_SIZE_BYTES             500000

// ======================== HTTP 配置 ========================
#define HTTP_UPLOAD_TIMEOUT_MS         10000

// ======================== 音量控制 ========================
#define VOLUME_DEFAULT                 100
#define VOLUME_STEP                    10
#define VOLUME_MIN                     0
#define VOLUME_MAX                     100

// ======================== 软件增益 ========================
#define SOFTWARE_GAIN                  3     // 3倍放大（仅用于Opus编码）

// ======================== 静默检测 ========================
#define VOICE_RMS_THRESHOLD            200   // RMS阈值（原始数据）
#define SILENCE_TIMEOUT_MS             15000 // 15秒静默超时

// ======================== 流播放 ========================
#define STREAM_QUEUE_DEPTH             64    // 队列深度（PSRAM分配）

// ======================== FreeRTOS 任务配置 ========================
#define TASK_WAKE_WORD_STACK_SIZE      16384
#define TASK_WAKE_WORD_PRIORITY        6
#define TASK_WAKE_WORD_CORE            1

#define TASK_RECORD_STACK_SIZE         32768
#define TASK_RECORD_PRIORITY           7
#define TASK_RECORD_CORE               1     // I2S DMA 绑定 Core 1

#define TASK_PLAY_STACK_SIZE           16384
#define TASK_PLAY_PRIORITY             5
#define TASK_PLAY_CORE                 0
```

### 4.4 完整会话流程

```
1. 设备启动
   → WiFi连接
   → MQTT连接 → publish {"status":"online"}
   → WebSocket连接 → send hello → 等待确认
   → 启动 stream_play_task（准备接收服务器音频）

2. 等待唤醒词 "你好小安"
   → wake_word_task 调用 wake_word_wait() 阻塞等待
   → AFE: [I2S stereo输入] → SE(BSS) → VAD → WakeNet(wn9_nihaoxiaoan_tts2)

3. 唤醒词检测到
   → audio_player_stop() 停止播放任务（销毁 stream_play_task + 释放队列）
   → s_last_voice_time_us = esp_timer_get_time()
   → audio_recorder_start_stream(on_audio_frame, ...) 启动流式录音
   → audio_player_stream_start() 启动新的播放任务

4. 流式录音（Core 1 stream_task）
   → I2S stereo 读取 → stereo→mono 提取左声道
   → RMS 计算（原始数据）→ 更新 s_last_voice_time_us
   → 软件增益 3x（仅用于编码）
   → Opus 编码 → BP3 帧 → WebSocket 发送

5. 静默检测（Core 1 wake_word_task）
   → 每 1 秒检查 esp_timer_get_time() - s_last_voice_time_us > 15000
   → 15 秒无语音 → voice_active = false
   → audio_recorder_stop_stream()
   → ws_uploader_send_text("stt_end")

6. 服务器处理
   → STT → LLM → TTS
   → MQTT publish home/agent/reply
   → WebSocket推送PCM BP3帧（type=0x01, 1920字节/帧）

7. 设备接收回复
   → on_ws_binary 接收 PCM 帧 → audio_player_stream_queue() 入队
   → stream_play_task → i2s_channel_write(TX) → ES8156 DAC → 扬声器
   → LCD显示回复文本

8. 返回步骤2
```

### 4.5 I2S 全双工架构

```
I2S0 全双工:
  TX (Speaker): STD Philips, 16kHz, 16bit, mono
    → DMA: 12描述符 × 480帧
    → esp_codec_dev_open(s_spk_handle) 配置 ES8156
    → 必须 i2s_channel_enable(tx_handle) 重新启用 TX
    → stream_play_task (Core 0) 调用 i2s_channel_write

  RX (Mic): STD Philips, 16kHz, 16bit, stereo
    → DMA: 6描述符 × 240帧
    → ES7243E 通过 I2C 配置（PGA 37.5dB 最大）
    → audio_feed_task (Core 1) 调用 i2s_channel_read
    → 提取左声道 mono[i] = stereo[i*2]

互斥保护:
  → FreeRTOS mutex (500ms 超时) 序列化 TX/RX 操作
  → audio_player_stop() 必须销毁 task + 释放队列，避免 DMA 并发死锁

关键发现:
  → esp_codec_dev_open(s_spk_handle) 内部 disable TX 再 reconfig
  → 必须手动 i2s_channel_enable(tx_handle) 重新启用
  → I2S DMA 中断绑定 Core 1，所有 I2S 任务必须在 Core 1
```

### 4.6 音频流协议 (BP3)

```
BinaryProtocol3 帧格式:
  Offset  Size  Type      Field
    0      1    uint8     type (0x00=Opus, 0x01=PCM)
    1      1    uint8     reserved (始终为0)
    2      2    uint16    payload_size (big-endian)
    4      N    uint8[]   payload

帧类型:
  0x00  设备→服务器  Opus编码音频（16kHz, mono, 60ms/帧）
  0x01  服务器→设备  PCM原始音频（16kHz, 16bit, mono, 1920字节/帧）

Opus编码参数:
  采样率: 16000 Hz
  通道: 1 (mono)
  位深: 16-bit signed
  帧时长: 60ms (960 samples)
  码率: AUTO (VBR)
  DTX: 开启（静音时不产生数据）

服务器音频推送:
  POST http://服务器:5000/api/audio/send
  {"path": "/path/to/audio.mp3"}
  → 服务器将MP3解码为PCM → WebSocket BP3帧(type=0x01) → ESP32入队播放
```

### 4.7 WebSocket 客户端

```c
// websocket_uploader.h
bool ws_uploader_connect(const char *url);
void ws_uploader_disconnect(void);
bool ws_uploader_is_connected(void);
bool ws_uploader_send_binary(const uint8_t *data, size_t len);
bool ws_uploader_send_text(const char *text);
void ws_uploader_set_on_text(ws_on_text_cb_t cb, void *ctx);
void ws_uploader_set_on_binary(ws_on_binary_cb_t cb, void *ctx);
```

实现要点:
  → 使用 ESP-IDF 内置 esp_transport_ws（tcp_transport 组件）
  → ws_recv_task 栈 8192（支持 Opus 解码在回调中执行）
  → disconnect 需先通知 recv task 退出（s_recv_running + esp_transport_close）
  → 回调架构：on_text 处理 JSON，on_binary 处理 BP3 帧

### 4.8 音频播放器

```c
// audio_player.h
void audio_player_set_codec(audio_codec_t *codec);
void audio_player_stream_start(void);       // 启动流播放任务（PSRAM队列64帧）
void audio_player_stop(void);               // 销毁任务+释放队列
bool audio_player_stream_queue(const int16_t *data, size_t samples);
void audio_player_set_volume(int volume);
int audio_player_get_volume(void);
```

实现要点:
  → stream_play_task 在 init 时启动，唤醒词检测前 audio_player_stop() 销毁
  → 流式录音开始后 audio_player_stream_start() 重建
  → 队列用 PSRAM 分配：64帧 × 1924字节 = 120KB
  → i2s_channel_write 500ms 超时阻塞播放
  → 播放帧日志：前5帧 + 每100帧

### 4.9 唤醒词检测

```c
// wake_word_detector.h
void wake_word_set_codec(audio_codec_t *codec);
bool wake_word_init(uint32_t sample_rate);
bool wake_word_wait(void);                   // 阻塞等待唤醒词
bool wake_word_is_voice_detected(void);
bool wake_word_detector_is_ready(void);
```

实现要点:
  → AFE 配置：MM模式（stereo输入），高性能模式
  → ES7243E PGA 增益 37.5dB（寄存器 0x20/0x21 = 0x1E）
  → 唤醒词模型：wn9_nihaoxiaoan_tts2（你好小安）
  → sdkconfig.defaults 中 CONFIG_SR_WN_WN9_NIHAOXIAOAN_TTS2=y
  → AFE Pipeline: [input] → SE(BSS) → VAD(WebRTC) → WakeNet → [output]
  → feed task 在 Core 1 运行（I2S DMA 绑定）

### 4.10 录音器

```c
// audio_recorder.h
typedef void (*audio_frame_cb_t)(const int16_t *data, int samples, void *ctx);
void audio_recorder_set_codec(audio_codec_t *codec);
void audio_recorder_start_stream(audio_frame_cb_t cb, void *ctx, int frame_samples);
void audio_recorder_stop_stream(void);
```

实现要点:
  → 流式模式：stream_task 循环读 I2S stereo，通过回调上传
  → stereo→mono：提取左声道 mono[i] = stereo[i*2]
  → RMS 计算：用原始数据（不受增益影响），阈值 200
  → 软件增益 3x：仅用于 Opus 编码，RMS 检测用原始数据
  → stream_task 必须在 Core 1（I2S DMA 绑定）

### 4.11 LCD 显示

```c
// lcd_display.h
typedef enum { CHAT_STATE_IDLE, CHAT_STATE_LISTENING, CHAT_STATE_THINKING, CHAT_STATE_SPEAKING } chat_state_t;
void lcd_chat_init(lcd_display_t *lcd);
void lcd_chat_set_state(lcd_display_t *lcd, chat_state_t state, const char *status);
void lcd_chat_add_message(lcd_display_t *lcd, int msg_type, const char *text);
```

实现要点:
  → 320x240 深色主题聊天 UI
  → 气泡消息，环形缓冲区 20 条
  → 仅支持 ASCII 字符
  → 全屏帧缓冲（150KB PSRAM）+ 脏标记刷新

## 5. 任务调度

```
FreeRTOS 任务分配:

  任务名             优先级  栈大小   核心   职责
  ──────────────────────────────────────────────────────────
  mainTask           1       8192    0     主循环（MQTT 保活 + Wi-Fi 重连）
  wake_word_task     6       16384   1     唤醒词等待 + 静默检测
  audio_feed_task    4       16384   1     AFE 音频输入（I2S stereo → AFE）
  stream_task        7       32768   1     流式录音（I2S → Opus → WebSocket）
  stream_play_task   5       16384   0     流式播放（队列 → i2s_channel_write）
  ws_recv_task       5       8192    0     WebSocket 接收（BP3 帧解析）
  MQTT 内部          5       4096    0     MQTT 消息处理 (esp-mqtt)
  WiFi 内部          4       4096    0     Wi-Fi 管理 (esp_wifi)
```

## 6. 内存分配

```
总可用: 512KB SRAM + 8MB Flash + 8MB PSRAM

分配:
  - FreeRTOS 堆: ~200KB
  - Wi-Fi + MQTT: ~50KB
  - LCD 帧缓冲: ~150KB (320x240x2, PSRAM)
  - 音频缓冲: ~160KB (15s * 16kHz * 16bit, PSRAM)
  - 流播放队列: 120KB (64帧 × 1924字节, PSRAM)
  - JSON 解析: ~4KB (cJSON)

PSRAM 使用:
  - 音频缓冲优先使用 heap_caps_malloc(MALLOC_CAP_SPIRAM) 分配到 PSRAM
  - FreeRTOS 队列用 xQueueCreateStatic + PSRAM 分配
  - ESP-SR 模型加载到 PSRAM (AFE_MEMORY_ALLOC_MORE_PSRAM)
  - LCD 帧缓冲 150KB 在 PSRAM
```

## 7. API 对照表（Arduino → ESP-IDF）

| 功能 | Arduino 实现 | ESP-IDF 原生 |
|------|-------------|-------------|
| WiFi | `WiFi.h` | `esp_wifi.h` + `esp_netif.h` |
| MQTT | `PubSubClient` | `esp_mqtt_client` (esp-mqtt) |
| HTTP | `HTTPClient.h` | `esp_http_client.h` |
| WebSocket | 纯socket | `esp_transport_ws` (tcp_transport) |
| JSON | `ArduinoJson v7` | `cJSON.h` |
| LCD | `LovyanGFX` | `esp_lcd_panel` + `esp_lcd_ili9341` |
| I2S | `driver/i2s.h` (旧版) | `driver/i2s_std.h` (新版) |
| GPIO | `Arduino digitalWrite` | `driver/gpio.h` |
| 日志 | `Serial.printf` | `ESP_LOGI/W/E` |
| 任务 | `xTaskCreatePinnedToCore` | 同 (FreeRTOS 原生) |
| 入口 | `setup() + loop()` | `app_main()` |
| 内存 | `ps_malloc()` | `heap_caps_malloc(MALLOC_CAP_SPIRAM)` |
| 音频编解码 | 手动 I2S 配置 | `esp_codec_dev` (ES8156+ES7243E) |
| 语音识别 | ESP-SR (手动配置) | ESP-SR AFE + WakeNet (官方 API) |
| 音频编码 | 无 | Opus (esp_opus_enc / 原生 libopus) |

## 8. 已知问题与解决方案

### 8.1 I2S DMA 并发死锁

问题：TX (Core 0 stream_play_task) 和 RX (Core 1 stream_task) 同时操作 I2S 导致系统冻结。

解决：
  → audio_player_stop() 必须销毁 task + 释放队列
  → FreeRTOS mutex (500ms 超时) 序列化 I2S 读写
  → stream_play_task 在 init 时启动，录音前 stop，录音后 start

### 8.2 esp_codec_dev_open 破坏 TX 通道

问题：esp_codec_dev_open(s_spk_handle) 内部 disable TX 再 reconfig，但不会自动 re-enable。

解决：init 中 esp_codec_dev_open 后必须手动 i2s_channel_enable(tx_handle)

### 8.3 ES7243E PGA 增益与唤醒距离

问题：默认增益下唤醒词检测距离 <30cm。

解决：PGA 设为最大 37.5dB（寄存器 0x20/0x21 = 0x1E），唤醒距离提升到 ~50cm。
软件增益 3x 仅用于 Opus 编码，RMS 检测用原始数据（避免环境噪声触发静默超时）。

### 8.4 服务器 PCM 帧误判为 JSON

问题：type=0x01 的 PCM 帧被当作 JSON 文本处理。

解决：on_ws_binary 中非 Opus 帧走 PCM 播放分支。

### 8.5 流播放队列溢出

问题：服务器一次性发送 500 帧，队列深度 16 不够，帧堆积导致播放卡顿。

解决：队列深度 64（120KB PSRAM），DMA 缓冲 12×480。

## 9. 源文件列表（27个文件，全部中文注释）

| 文件 | 行数 | 说明 |
|------|------|------|
| `main.c` | ~570 | 主入口，WebSocket回调，唤醒词任务 |
| `config.h` | ~150 | 全局配置宏 |
| `audio_codec.c/h` | ~265 | I2S初始化，ES8156/ES7243E编解码器 |
| `audio_player.c/h` | ~200 | 流播放任务，音量控制 |
| `audio_recorder.c/h` | ~250 | 缓冲录音，流式录音 |
| `audio_stream_protocol.h` | ~30 | BP3帧格式定义 |
| `opus_encoder.c/h` | ~100 | Opus编码器 |
| `opus_decoder.c/h` | ~120 | Opus解码器（原生libopus） |
| `websocket_uploader.c/h` | ~300 | WebSocket客户端 |
| `app_mqtt_client.c/h` | ~200 | MQTT客户端 |
| `wifi_manager.c/h` | ~80 | WiFi连接管理 |
| `wake_word_detector.c/h` | ~185 | 唤醒词检测（ESP-SR AFE+WakeNet） |
| `http_uploader.c/h` | ~80 | HTTP客户端 |
| `lcd_display.c/h` | ~792 | LCD显示（ILI9341，聊天UI） |
| `codec_benchmark.c/h` | ~50 | 编解码器性能测试 |
