# ESP32 Box Lite — 各模块实现流程与问题记录

> 本文档详细记录每个源文件的实现流程、关键设计决策、以及开发过程中遇到的问题和解决方案。

---

## 目录

1. [config.h — 全局配置](#1-configh)
2. [main.c — 主入口](#2-mainc)
3. [audio_codec.c/h — 音频编解码器](#3-audio_codec)
4. [audio_player.c/h — 音频播放器](#4-audio_player)
5. [audio_recorder.c/h — 音频录制器](#5-audio_recorder)
6. [audio_stream_protocol.h — BP3协议](#6-audio_stream_protocol)
7. [opus_encoder.c/h — Opus编码器](#7-opus_encoder)
8. [opus_decoder.c/h — Opus解码器](#8-opus_decoder)
9. [websocket_uploader.c/h — WebSocket客户端](#9-websocket_uploader)
10. [app_mqtt_client.c/h — MQTT客户端](#10-app_mqtt_client)
11. [wifi_manager.c/h — WiFi管理](#11-wifi_manager)
12. [wake_word_detector.c/h — 唤醒词检测](#12-wake_word_detector)
13. [lcd_display.c/h — LCD显示](#13-lcd_display)
14. [http_uploader.c/h — HTTP客户端](#14-http_uploader)
15. [codec_benchmark.c/h — 编解码器测试](#15-codec_benchmark)

---

## 1. config.h

### 实现流程
全局配置头文件，定义所有硬件引脚、网络参数、音频参数、FreeRTOS任务配置。

### 关键配置项

```c
// I2S引脚（与设计文档一致）
I2S_MCLK=38, I2S_BCLK=14, I2S_WS=13, I2S_DOUT=11, I2S_DIN=12

// 服务器
SERVER_HOST="82.158.224.81", MQTT_PORT=1883, WS_PORT=8888, HTTP_PORT=5000

// 音频
AUDIO_INPUT_SAMPLE_RATE=16000, VOLUME_DEFAULT=100
SOFTWARE_GAIN=3, VOICE_RMS_THRESHOLD=200, SILENCE_TIMEOUT_MS=15000

// 任务
TASK_RECORD_CORE=1, TASK_RECORD_STACK_SIZE=32768
STREAM_QUEUE_DEPTH=64
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| I2S DMA 中断绑定 Core 1 | ESP32-S3 硬件限制 | `TASK_RECORD_CORE` 必须设为 1 |
| 环境噪声触发静默超时 | RMS 阈值太低(50) | 提高到 200 |
| 软件增益后静默超时失效 | 3x增益后噪声RMS>阈值 | RMS用原始数据，增益仅用于编码 |
| 默认音量太小 | VOLUME_DEFAULT=50 | 改为 100 |

---

## 2. main.c

### 实现流程

```
app_main()
  ├── [1] I2C 初始化 (i2c_new_master_bus)
  ├── [2] LCD 初始化 (lcd_chat_init)
  ├── [3] 音频编解码器初始化 (audio_codec_init)
  │     ├── I2S TX/RX 通道创建
  │     ├── ES8156 DAC 初始化 (speaker codec device)
  │     ├── ES7243E ADC 初始化 (I2C only, 绕过 esp_codec_dev)
  │     └── i2s_channel_enable(tx_handle) ← 关键！
  ├── [4] WiFi 连接
  ├── [5] MQTT 连接 + 回调注册
  ├── [6] Opus 编码器/解码器初始化
  ├── [7] WebSocket 连接 + 回调注册
  ├── [8] 唤醒词检测初始化
  ├── [9] 流播放任务启动 (audio_player_stream_start)
  ├── [10] 唤醒词任务启动 (wake_word_task, Core 1)
  └── [11] 按键任务启动 (button_task, Core 0)

主循环:
  while (true) {
      wifi_manager_check_reconnect();
      vTaskDelay(1000ms);
  }
```

### wake_word_task 流程

```
wake_word_task (Core 1):
  loop:
    1. 检查 WebSocket 连接状态
    2. lcd_chat_set_state(IDLE, "Waiting for wake word...")
    3. wake_word_wait() — 阻塞等待唤醒词
    4. audio_player_stop() — 销毁 stream_play_task + 释放队列
    5. s_last_voice_time_us = esp_timer_get_time()
    6. audio_recorder_start_stream(on_audio_frame, ...)
    7. audio_player_stream_start() — 重建播放任务+队列
    8. 静默检测循环 (每1秒检查):
       if (esp_timer_get_time() - s_last_voice_time_us > 15000ms)
         → voice_active = false
    9. audio_recorder_stop_stream()
    10. ws_uploader_send_text("stt_end")
    11. 回到步骤1
```

### on_ws_binary 回调流程

```
on_ws_binary(data, len):
  1. 解析 BP3 头: type, payload_size
  2. if (type == 0x00)  // Opus帧
       → app_opus_decoder_decode() → audio_player_stream_queue()
     else  // PCM帧 (type=0x01, 服务器推送)
       → audio_player_stream_queue(pcm_data, pcm_samples)
```

### on_audio_frame 回调流程

```
on_audio_frame(stereo_data, samples):
  1. stereo→mono: mono[i] = stereo[i*2]
  2. RMS 计算（原始数据，不受增益影响）
     → if RMS > 200: 更新 s_last_voice_time_us
  3. 软件增益 3x: mono[i] *= SOFTWARE_GAIN
  4. app_opus_encoder_encode() → BP3帧 → ws_uploader_send_binary()
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `esp_timer_get_time` 隐式声明 | 缺少头文件 | 添加 `#include "esp_timer.h"` |
| `on_ws_binary` 代码重复 | 编辑时产生重复的 else if 块 | 删除重复代码 |
| PCM 帧被误判为 JSON | type=0x01 走 JSON 分支 | 添加 else 分支直接入队播放 |
| `audio_player_stream_start()` 时机 | init时启动与录音DMA冲突 | 改为唤醒词检测后启动 |
| 15秒静默超时不生效 | AFE VAD 在 feed task 退出后不可用 | 改用 RMS + esp_timer_get_time |

---

## 3. audio_codec.c/h

### 实现流程

```
audio_codec_init(i2c_bus, pa_pin, mclk, bclk, ws, dout, din):
  1. 创建 I2S 通道 (i2s_new_channel)
     ├── TX: 12描述符 × 480帧
     └── RX: 6描述符 × 240帧
  2. TX 配置: STD Philips, 16kHz, 16bit, MONO
  3. RX 配置: STD Philips, 16kHz, 16bit, STEREO
  4. I2S 通道使能 (i2s_channel_enable)
  5. I2C 控制接口创建 (codec_new_i2c_wired)
  6. GPIO 接口创建 (codec_new_gpio)
  7. ES7243E ADC 初始化 (es7243e_codec_new)
     → 仅 I2C 配置，不用 esp_codec_dev 管理 I2S
  8. ES8156 DAC 初始化 (es8156_codec_new)
  9. Speaker codec device 创建:
     → esp_codec_dev_new(spk_dev_cfg)
     → esp_codec_dev_open(s_spk_handle, &spk_fs)
     → esp_codec_dev_set_out_vol(100)
     → esp_codec_dev_set_out_mute(false)
     → i2s_channel_enable(tx_handle) ← 关键！重新启用TX
  10. PA 引脚使能 (gpio_set_level(pa_pin, 1))
  11. FreeRTOS mutex 创建 (500ms 超时)

audio_codec_input(codec, data, samples):
  → mutex lock → i2s_channel_read(rx_handle) → mutex unlock

audio_codec_output(codec, data, samples):
  → 检查 output_enabled → i2s_channel_write(tx_handle) → 返回结果
```

### 关键设计决策

1. **绕过 esp_codec_dev 管理 I2S RX**：`esp_codec_dev_open(s_mic_handle)` 会调用 `_i2s_data_set_fmt()` 破坏 TDM RX DMA 配置。mic 只用 I2C 配置 ES7243E 寄存器，I2S 直接用 `i2s_channel_read`。

2. **Speaker 用 esp_codec_dev**：ES8156 需要 `esp_codec_dev_open` 完整配置（I2C 寄存器 + I2S 格式），但之后必须手动 `i2s_channel_enable(tx_handle)`。

3. **I2S slot 配置用显式结构体**：ESP32-S3 的 `i2s_std_slot_config_t` 用 `left_align`/`big_endian`/`bit_order_lsb`（不是 `msb_right`），IntelliSense 对宏展开有误报。

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `esp_codec_dev_open(s_mic_handle)` 破坏 RX DMA | `_i2s_data_set_fmt()` 重新配置 TDM RX | mic 跳过 esp_codec_dev，只用 I2C |
| TX/RX DMA 并发死锁 | Core 0 TX + Core 1 RX 同时操作 | FreeRTOS mutex (500ms 超时) |
| `i2s_channel_write` 返回 "channel not enabled" | `esp_codec_dev_open` 内部 disable TX | 之后手动 `i2s_channel_enable(tx_handle)` |
| `esp_codec_dev_write` 返回成功但无声 | TX 通道未正确配置 | 改用直接 `i2s_channel_write` |
| IntelliSense 报 "left_align" 错误 | 宏展开包含 ESP32-S3 不支持的字段 | 用显式结构体替代宏 |
| FreeRTOS mutex portMAX_DELAY 导致 boot loop | mutex 被持有后未释放 | 改为 500ms 超时 |
| ES7243E PGA 增益不够 | 默认增益下唤醒距离 <30cm | PGA 设为最大 37.5dB (0x1E) |
| speaker codec device 创建时 tx_handle 未定义 | 之前移除了静态变量 | 改用 `codec->tx_handle` |

---

## 4. audio_player.c/h

### 实现流程

```
audio_player_stream_start():
  1. 创建 PSRAM 队列 (64帧 × 1924字节 = 120KB)
     → heap_caps_malloc(MALLOC_CAP_SPIRAM)
     → xQueueCreateStatic()
  2. 创建 stream_play_task (Core 0, 优先级5, 栈16384)

stream_play_task:
  loop:
    1. xQueueReceive(s_stream_queue, &frame, 200ms)
    2. if 收到帧:
         → audio_codec_enable_output(true)
         → audio_codec_output(frame.data, frame.samples)
         → 日志: 前5帧 + 每100帧
    3. else:
         → s_playing = false

audio_player_stop():
  1. s_stop_flag = true
  2. audio_codec_enable_output(false)
  3. vTaskDelete(s_stream_task) ← 关键！必须销毁任务
  4. vQueueDelete(s_stream_queue)
  5. 释放队列存储内存 (heap_caps_free)
  6. s_stream_queue = NULL, s_stream_task = NULL

audio_player_stream_queue(data, samples):
  → 检查 s_stream_queue != NULL
  → 封装为 pcm_frame_t
  → xQueueSend(s_stream_queue, ..., 0)  // 非阻塞
  → 队列满时丢帧并打印警告
```

### 关键设计决策

1. **PSRAM 队列**：内部 RAM ~251KB 不够，用 `xQueueCreateStatic` + `heap_caps_malloc(MALLOC_CAP_SPIRAM)` 分配存储。

2. **audio_player_stop 必须销毁任务**：旧代码只设标志 + 重置队列，任务仍存活。下次录音时 TX/RX DMA 并发死锁。新代码 `vTaskDelete` + 释放队列。

3. **stream_play_task 在 init 时启动**：之前延迟到录音结束后启动，但测试发现 init 时启动也能正常工作（`audio_player_stop` 在唤醒词检测前正确销毁任务）。

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| DMA 并发死锁 | stream_play_task 未销毁，与录音任务同时操作 I2S | `audio_player_stop` 中 `vTaskDelete` |
| 队列溢出 | 深度4太小，服务器一次发500帧 | 深度改为64 |
| 帧堆积播放卡顿 | DMA 缓冲太小(6×240) | 改为 12×480 |
| `audio_player_stream_queue` 依赖 start | 队列只在 start 中创建 | 确保 start 在 stop 之后调用 |
| 内部 RAM 不足 | 64帧 × 1924字节 > 251KB | 用 PSRAM 分配 |

---

## 5. audio_recorder.c/h

### 实现流程

```
audio_recorder_start_stream(cb, ctx, frame_samples):
  1. 保存回调函数指针
  2. 创建 stream_task (Core 1, 优先级7, 栈32768)

stream_task (Core 1):
  loop (while s_streaming):
    1. audio_codec_input(stereo_buf, frame_samples*2)
       → i2s_channel_read(rx_handle) 读 stereo 数据
    2. stereo→mono: mono[i] = stereo[i*2]
    3. 调用回调: on_audio_frame(mono, frame_samples)

audio_recorder_stop_stream():
  1. s_streaming = false
  2. 等待 stream_task 退出 (eTaskGetState 检查)
  3. vTaskDelay(100ms) 等待 I2S DMA 空闲
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| stream_task 创建后 crash | 可能栈溢出或 Core 调度问题 | 栈增大到 32768，确认 Core 1 |
| stereo→mono 位置错误 | 在 I2S 读取层做 mono 提取 | 改在回调中提取左声道 |
| I2S RX 必须用 STEREO | ES7243E 输出 stereo，用 "MM" 模式 | RX 配置为 STEREO |
| stop 后 I2S DMA 未空闲 | 立即开始下次录音导致冲突 | vTaskDelay(100ms) 等待 |

---

## 6. audio_stream_protocol.h

### 实现流程

定义 BP3 (Binary Protocol V3) 帧格式：

```c
帧头 (4字节):
  [0] type: uint8      — 0x00=Opus, 0x01=PCM
  [1] reserved: uint8  — 始终为0
  [2-3] payload_size: uint16 (big-endian)

常量:
  AUDIO_STREAM_FRAME_DURATION_MS = 60
  AUDIO_STREAM_SAMPLE_RATE = 16000
  AUDIO_STREAM_FRAME_SAMPLES = 960
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| type=0x01 含义不一致 | 服务器发PCM用0x01，协议定义为JSON | 更新协议：0x01=PCM |
| BP3 头解析错误 | payload_size 是 big-endian | `((data[2]<<8)|data[3])` |

---

## 7. opus_encoder.c/h

### 实现流程

```
app_opus_encoder_init(sample_rate, channels, frame_duration_ms):
  1. 计算 frame_size = sample_rate * duration / 1000
  2. esp_opus_enc_create() 创建编码器
     → config: 16kHz, mono, VOIP 模式, 60ms 帧
  3. esp_opus_enc_open() 打开编码器

app_opus_encoder_encode(pcm_data, pcm_samples):
  1. esp_opus_enc_process(encoder, pcm, frame_size, out_buf, &out_size)
  2. 返回编码后的字节数

app_opus_encoder_destroy():
  → esp_opus_enc_close() + esp_opus_enc_destroy()
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| 编码后数据过大 | 码率设置过高 | 使用 VBR + DTX |
| 编码延迟 | 60ms 帧时长 | 可接受，平衡延迟与压缩率 |

---

## 8. opus_decoder.c/h

### 实现流程

```
app_opus_decoder_init(sample_rate, channels):
  1. opus_decoder_get_size() 获取内存大小
  2. heap_caps_malloc(MALLOC_CAP_SPIRAM) 分配
  3. opus_decoder_init() 初始化

app_opus_decoder_decode(enc_data, enc_len, pcm_out, max_samples):
  1. opus_decode(decoder, enc_data, enc_len, pcm_out, max_samples, 0)
  2. 返回解码的样本数

app_opus_decoder_destroy():
  → heap_caps_free(decoder)
```

### 关键设计决策

**绕过 esp_opus_dec 封装层，直接用原生 libopus API**：`esp_opus_dec_decode()` 返回 -5，与服务器 `opus_encode()` 输出不兼容。直接调用 `opus_decoder_get_size` + `opus_decoder_init` + `opus_decode`，符号已导出在 `libesp_audio_codec.a` 中。

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| `esp_opus_dec_decode` 返回 -5 | 服务器 opus_encode 与 ESP 封装层不兼容 | 绕过封装，直接用原生 libopus |
| 解码器内存分配失败 | 内部 RAM 不足 | 用 PSRAM 分配 |
| 服务器下发的是 PCM 不是 Opus | `send_mp3_to_devices` 发 PCM | 添加 PCM 直接播放分支 |

---

## 9. websocket_uploader.c/h

### 实现流程

```
ws_uploader_connect(url):
  1. esp_transport_tcp_create() 创建 TCP transport
  2. esp_transport_ws_create() 创建 WebSocket transport
  3. esp_transport_ws_set_upgrade_config() 设置路径
  4. esp_transport_connect() 连接服务器
  5. 发送 hello 握手消息 (JSON)
  6. 创建 ws_recv_task (Core 0, 栈8192)

ws_recv_task:
  loop (while s_recv_running):
    1. esp_transport_read(ws, buf, 2048, 500ms)
    2. if opcode == TEXT → s_on_text_cb(buf)
    3. if opcode == BINARY → s_on_binary_cb(buf)
    4. if opcode == PING → 发送 PONG

ws_uploader_send_binary(data, len):
  → 封装为 BP3 帧 → esp_transport_ws_send_raw(BINARY)

ws_uploader_send_text(text):
  → esp_transport_ws_send_raw(TEXT)

ws_uploader_disconnect():
  1. s_recv_running = false
  2. esp_transport_close() — 打断阻塞读
  3. 等待 recv_task 退出 (轮询200ms)
  4. esp_transport_destroy()
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| ws_recv_task 栈溢出 | Opus 解码在回调中执行，栈4096不够 | 栈增大到 8192 |
| disconnect 时 use-after-free | recv task 在 `esp_transport_read` 中时销毁 transport | `s_recv_running` + `esp_transport_close` + 等待退出 |
| WebSocket 频繁断连 | 服务器 mosquitto 配置问题 | 检查 broker 配置 |

---

## 10. app_mqtt_client.c/h

### 实现流程

```
mqtt_client_init(host, port):
  1. 构建 URI: "mqtt://host:port"
  2. esp_mqtt_client_init() 配置:
     → client_id, will message (offline), reconnect timeout
  3. esp_mqtt_client_register_event() 注册事件回调
  4. esp_mqtt_client_start()

mqtt_event_handler:
  CONNECTED → subscribe_all() + publish online
  DATA → handle_message(topic, data)
  DISCONNECTED → log warning

handle_message(topic, data):
  → cJSON_Parse()
  → 根据 topic 分发到回调:
     agent/reply → s_reply_cb(user, text, tts_url)
     agent/identity → s_identity_cb(user, confidence)
     alert → s_alert_cb()
     music/status → s_music_cb(state, track, artist)
     agent/skill → s_skill_cb(user, skill)
  → cJSON_Delete()
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| MQTT 频繁断连重连 | mosquitto broker 配置问题 | 检查 broker 版本兼容性 |
| `handle_message` 用 `strstr` 匹配 | 可能误匹配子串 | 用完整 topic 比较 |

---

## 11. wifi_manager.c/h

### 实现流程

```
wifi_manager_init():
  1. nvs_flash_init() — NVS 存储
  2. esp_netif_init() — 网络接口
  3. esp_event_loop_create_default() — 事件循环
  4. esp_netif_create_default_wifi_sta()
  5. esp_wifi_init()
  6. 注册事件回调 (WIFI_EVENT, IP_EVENT)
  7. esp_wifi_set_mode(STA)
  8. esp_wifi_start()

wifi_manager_connect(ssid, password):
  → wifi_config_t 填充 → esp_wifi_set_config → esp_wifi_connect

事件回调:
  STA_START → esp_wifi_connect()
  STA_DISCONNECTED → s_connected=false, esp_wifi_connect()
  GOT_IP → s_connected=true, 记录 IP
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| NVS 初始化在 wifi_manager 中 | 应该在 main.c | 保持在 wifi_manager（兼容性） |
| WiFi 断开后不自动重连 | 事件回调未注册 | 在 STA_DISCONNECTED 中调用 connect |

---

## 12. wake_word_detector.c/h

### 实现流程

```
wake_word_init(sample_rate):
  1. esp_srmodel_init("model") 加载 SR 模型
  2. esp_srmodel_filter(ESP_WN_PREFIX) 查找唤醒词模型
     → wn9_nihaoxiaoan_tts2 (你好小安)
  3. afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_HIGH_PERF)
     → "MM" 模式：stereo 输入（ES7243E 双麦克风）
  4. 配置 AFE 参数:
     → aec_init=false (无硬件参考信号)
     → se_init=true (信号增强)
     → vad_init=true (语音活动检测)
     → wakenet_init=true
     → afe_perferred_core=1
     → memory_alloc_mode=AFE_MEMORY_ALLOC_MORE_PSRAM
  5. esp_afe_handle_from_config() 创建 AFE 句柄
  6. create_from_config() 创建 AFE 数据

wake_word_wait():
  → 创建 audio_feed_task (Core 1, 栈16384)
  → feed_task 循环:
       1. audio_codec_input(stereo_buf, chunk_samples*2)
       2. stereo→mono: mono[i] = stereo[i*2]
       3. afe->feed(afe_data, mono)
  → 主线程循环:
       1. afe->fetch_with_delay(afe_data, 100ms)
       2. if wakeup_state == WAKENET_DETECTED → return true
```

### 关键设计决策

1. **AFE 用 "MM" 模式**：ES7243E 输出 stereo，必须用 "MM" 不是 "M"。

2. **feed task 在 Core 1**：I2S DMA 绑定 Core 1，feed task 必须在 Core 1 运行。

3. **PGA 增益 37.5dB**：ES7243E 寄存器 0x20/0x21 = 0x1E (0x10 | 0x0E)。

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| AFE 用 "M" 模式无数据 | ES7243E 输出 stereo | 改为 "MM" 模式 |
| Mic RMS 仅 2-13 | ES7243E PGA 增益太低 | 设为 30.0f (37.5dB) |
| 唤醒距离 <50cm | PGA 已最大，软件增益不够 | 保持现状，可能需硬件改进 |
| feed task 在 Core 0 冻结 | I2S DMA 绑定 Core 1 | feed task 改到 Core 1 |
| AFE VAD 不能用于静默检测 | feed task 退出后无数据 | 改用 RMS + esp_timer |

---

## 13. lcd_display.c/h

### 实现流程

```
lcd_chat_init():
  1. SPI 总线初始化 (spi_bus_initialize)
  2. ILI9341 面板创建 (esp_lcd_new_panel_ili9341)
  3. 面板初始化 + 使能
  4. 背光使能 (GPIO45 LOW=亮)
  5. 全屏帧缓冲分配 (150KB PSRAM)
  6. 绘制初始界面

lcd_chat_add_message(msg_type, text):
  1. 添加到环形缓冲区 (20条)
  2. 标记脏区域
  3. lcd_chat_flush() 重绘

lcd_chat_flush():
  1. 绘制深色背景
  2. 绘制状态栏
  3. 绘制气泡消息 (用户右侧蓝色，AI左侧灰色)
  4. 绘制输入框
  5. esp_lcd_panel_draw_bitmap() 刷新到 LCD
```

### 关键设计决策

1. **全屏帧缓冲**：320x240x2 = 150KB 在 PSRAM，避免逐行发送闪烁。

2. **8x16 ASCII 字体**：内嵌字体数组，仅支持 ASCII。

3. **深色主题**：背景 #1A1A2E，用户消息蓝色，AI消息灰色。

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| LCD 闪烁 | 全屏 DMA 传输 153600B 太大 | 脏标记+定时刷新 |
| ILI9341 颜色错位 | 字节序问题 | swap_color(c) 大端序交换 |
| 字符显示不全 | 仅支持 ASCII | 保持 ASCII，中文用占位符 |
| God Object (792行) | LCD+UI+字体全在一个文件 | 后续可拆分 |

---

## 14. http_uploader.c/h

### 实现流程

```
http_post_json(host, port, path, json, response, max_len):
  1. 构建 URL: "http://host:port/path"
  2. esp_http_client_init() 配置
  3. esp_http_client_set_method(POST)
  4. esp_http_client_set_header(Content-Type: application/json)
  5. esp_http_client_set_post_field(json)
  6. esp_http_client_perform() 执行
  7. esp_http_client_read_response() 读取响应
  8. esp_http_client_cleanup()
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| HTTP 上传被 WebSocket 替代 | 流式上传更高效 | 保留作为备用 |

---

## 15. codec_benchmark.c/h

### 实现流程

```
codec_benchmark_run():
  阶段1: 静默测试 — 读取1秒I2S数据，计算RMS
  阶段2: 播放测试 — 生成正弦波 → i2s_channel_write
  阶段3: 回环测试 — 读取 → 增益 → 写入（需DOUT→DIN物理连接）
  阶段4: 麦克风测试 — 录制3秒，计算RMS
```

### 遇到的问题

| 问题 | 原因 | 解决方案 |
|------|------|----------|
| 回环测试需要硬件连接 | DOUT 和 DIN 未短接 | 跳过或手动连接 |

---

## 附录：关键经验总结

### I2S 全双工最佳实践

1. **TX/RX 必须在同一 Core 操作**：DMA 中断绑定 Core 1
2. **FreeRTOS mutex 序列化**：500ms 超时，避免 portMAX_DELAY
3. **esp_codec_dev_open 会破坏 RX DMA**：mic 绕过 esp_codec_dev
4. **esp_codec_dev_open 后必须 i2s_channel_enable(tx)**：手动重新启用
5. **audio_player_stop 必须销毁任务**：否则下次录音 DMA 死锁

### 音频流播放最佳实践

1. **PSRAM 队列**：64帧深度，避免内部 RAM 不足
2. **DMA 缓冲 12×480**：避免帧堆积
3. **init 时启动播放任务**：录音前 stop，录音后 start
4. **PCM 帧直接入队**：不需要 Opus 解码

### 唤醒词检测最佳实践

1. **AFE 用 "MM" 模式**：ES7243E stereo 输出
2. **PGA 增益 37.5dB**：最大灵敏度
3. **feed task 在 Core 1**：I2S DMA 绑定
4. **RMS 静默检测**：替代 AFE VAD（feed task 退出后不可用）
5. **RMS 用原始数据**：不受软件增益影响
