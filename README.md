# ESP32-S3-BOX-Lite 智能家居终端

## 项目概述

基于 ESP32-S3-BOX-Lite 开发板的智能家居语音终端，支持：
- 语音唤醒词检测（"你好小智"）
- 录音 + Opus 编码 + WebSocket 上传到服务器
- 服务器下发音频播放
- LCD 聊天界面显示
- MQTT 设备控制

## 硬件架构

```
┌─────────────────────────────────────────────┐
│           ESP32-S3-BOX-Lite                  │
│                                              │
│  ┌──────────┐    I2S TX (STD MONO)    ┌────┐│
│  │ ES8156   │◄────────────────────────►│    ││
│  │ (DAC)    │    GPIO15 (DOUT)         │    ││
│  └──────────┘                          │ E  ││
│       ▲ I2C (0x08)                     │ S  ││
│       │                               │ P  ││
│  ┌────┴─────┐    I2S RX (STD STEREO)  │ 3  ││
│  │ ES7243E  │◄────────────────────────►│ 2  ││
│  │ (ADC)    │    GPIO16 (DIN)          │ -  ││
│  └──────────┘                          │ S  ││
│       ▲ I2C (0x10)                     │ 3  ││
│       │                               └────┘│
│  ┌──────────┐                               │
│  │ ILI9341  │ SPI LCD (320x240)             │
│  └──────────┘                               │
│                                              │
│  GPIO2=MCLK, GPIO17=BCLK, GPIO47=WS         │
│  GPIO46=PA功放, GPIO45=LCD背光              │
└─────────────────────────────────────────────┘
```

## 软件架构

### 模块依赖关系

```
┌─────────────────────────────────────────────┐
│                   main.c                     │
│  应用入口 · MQTT/WS 回调 · 按键任务 · 主循环  │
└──────┬──────┬──────┬──────┬──────┬──────────┘
       │      │      │      │      │
       ▼      ▼      ▼      ▼      ▼
┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────────────┐
│wifi_mgr  │ │mqtt_client│ │ws_uploader│ │   audio_codec    │
│ WiFi STA │ │ MQTT 3.1 │ │ WebSocket │ │ I2S + I2C 驱动   │
└──────────┘ └──────────┘ └──────────┘ └───────┬──────────┘
                                                │
                          ┌─────────────────────┼─────────────────┐
                          ▼                     ▼                 ▼
                   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
                   │ audio_player │  │audio_recorder│  │wake_word_det │
                   │ 播放:URL/PCM │  │ 录音/流式录制 │  │ AFE+WakeNet  │
                   │ 流式队列播放 │  │ 回调上报帧   │  │ 唤醒词检测   │
                   └──────┬───────┘  └──────┬───────┘  └──────┬───────┘
                          │                 │                  │
                          ▼                 ▼                  ▼
                   ┌──────────────┐  ┌──────────────┐  ┌──────────────┐
                   │ opus_encoder │  │opus_decoder  │  │esp-sr (AFE)  │
                   │ esp_opus_enc │  │ 原生 libopus │  │ WakeNet模型  │
                   └──────────────┘  └──────────────┘  └──────────────┘
                          │                 │
                          ▼                 ▼
                   ┌──────────────────────────────┐
                   │     audio_stream_protocol     │
                   │   BP3 帧格式 · Opus 常量      │
                   └──────────────────────────────┘
```

### 数据流

```
录音流程：
  I2S RX (stereo) → audio_codec_input() → stream_task → on_audio_frame()
    → stereo→mono 提取左声道 → Opus 编码 → BP3 帧 → WebSocket 上传

播放流程：
  WebSocket 接收 → on_ws_binary() → Opus 解码 → audio_player_stream_queue()
    → stream_play_task → audio_codec_output() → I2S TX

唤醒词流程：
  I2S RX → audio_codec_input() → AFE feed → fetch → WakeNet 检测
    → 检测到后启动录音流程
```

## 关键设计决策

### 1. I2S DMA 绑定 Core 1

ESP32-S3 的 I2S DMA 中断绑定在 Core 1。所有 I2S 读写任务必须在 Core 1 运行：
- `audio_feed_task`（唤醒词音频输入）：Core 1
- `stream_task`（流式录音）：Core 1

在 Core 0 调用 `i2s_channel_read()` 会导致 DMA 死锁。

### 2. 不使用 esp_codec_dev 管理 I2S

`esp_codec_dev_open()` 内部的 `_i2s_data_set_fmt()` 会重新配置 I2S DMA，导致系统冻结。解决方案：
- ES8156 (DAC) 和 ES7243E (ADC) 只通过 I2C 配置寄存器
- I2S 数据直接用 `i2s_channel_read()` / `i2s_channel_write()` 读写

### 3. Feed/Fetch 双任务架构

AFE 的 feed（喂数据）和 fetch（取结果）必须在不同任务中运行：
- `audio_feed_task`：读取 I2S stereo 数据 → feed 给 AFE
- `wake_word_wait()`：`fetch_with_delay(portMAX_DELAY)` 阻塞等待

### 4. Stereo→Mono 转换

I2S RX 配置为 STEREO（2 通道），但 Opus 编码器配置为 MONO：
- `stream_task` 读取 `frame_samples * 2` 个 stereo 样本
- `on_audio_frame` 提取左声道 `pcm[i*2]` 后再 Opus 编码

## 编译和烧录

```bash
# 激活 ESP-IDF 环境
source ~/.espressif/v6.0.1/esp-idf/export.sh

# 编译
idf.py build

# 烧录（需要先按 BOOT + RESET 进入下载模式）
idf.py -p /dev/cu.usbmodem1101 flash

# 监控串口
idf.py -p /dev/cu.usbmodem1101 monitor
```

## 测试

### 1. 唤醒词检测

1. 烧录固件，等待 WiFi/WebSocket 连接
2. 对设备说 "你好小智"
3. 串口应显示 `Wake word detected!`

### 2. 音频上传

1. 唤醒词检测后，说几句话
2. 串口应显示 `Audio frame #N: mono=960 enc=XX sent=1`
3. 服务器日志应显示 `Received Opus frame: XX bytes`

### 3. 静默超时

1. 唤醒词检测后，保持安静 15 秒
2. 串口应显示 `stt_end` 消息
3. 系统回到等待唤醒词状态

## 服务器 API

- WebSocket: `ws://82.158.224.81:8888/ws`
- HTTP: `http://82.158.224.81:5000/`
- MQTT: `82.158.224.81:1883`

### 推送音频到 ESP32

```bash
curl -X POST http://82.158.224.81:5000/api/audio/send \
  -H "Content-Type: application/json" \
  -d '{"path": "/opt/smart_butler/output_high.mp3"}'
```

## 文件清单

| 文件 | 行数 | 功能 |
|------|------|------|
| `config.h` | 109 | 全局配置（引脚、网络、音频参数） |
| `main.c` | 487 | 应用入口、回调、任务 |
| `audio_codec.c` | 158 | I2S + I2C 音频驱动 |
| `audio_player.c` | 171 | 音频播放（URL/PCM/流式） |
| `audio_recorder.c` | 137 | 音频录制（缓冲/流式） |
| `wake_word_detector.c` | 130 | 唤醒词检测（AFE+WakeNet） |
| `opus_encoder.c` | 83 | Opus 编码器 |
| `opus_decoder.c` | 69 | Opus 解码器 |
| `audio_stream_protocol.h` | 39 | BP3 协议定义 |
| `websocket_uploader.c` | 169 | WebSocket 通信 |
| `app_mqtt_client.c` | 125 | MQTT 客户端 |
| `wifi_manager.c` | 77 | WiFi 连接管理 |
| `lcd_display.c` | 792 | LCD 显示（聊天 UI） |
| `http_uploader.c` | 41 | HTTP 上传 |
| `codec_benchmark.c` | 128 | 音频硬件测试 |